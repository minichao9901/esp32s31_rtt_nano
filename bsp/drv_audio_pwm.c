/*===========================================================================
 * drv_audio_pwm.c -- 用 LEDC PWM 做的"喇叭"（RT-Thread audio 框架的放音设备）
 *
 * 为什么是 PWM（而不是 DAC/I2S）
 *   ESP32-S31 **没有 DAC**（soc_caps 里连 SOC_DAC_SUPPORTED 都没有），
 *   所以"用内置外设直接出声"这条路只剩 PWM/SDM。选 LEDC PWM 的理由：
 *     · 只需要**一个任意 GPIO** + 一个 RC 低通（截止频率放在 fs/2 与载波之间）；
 *     · 8 位分辨率 @156.25kHz 载波（XTAL 40MHz ÷ 2^8）→ 16kHz 采样率下
 *       每个采样点还有 ~10 个载波周期，平均值就是样本值，够放 8/16 位 WAV；
 *     · 载波远高于音频带，RC 一滤就干净；底噪主要是载波残余与量化噪声。
 *
 * 数据通路（**完全走 RT-Thread 官方 audio 框架**，见 components/drivers/audio/dev_audio.c）
 *
 *     应用 rt_device_write(sound0, ...)                      ← 我们把 WAV 喂进来
 *       └─ _audio_dev_write：切块进 memory pool + data queue
 *            └─ ops->start(REPLAY)：**本文件**打开采样时钟（SYSTIMER TARGET1）
 *            └─ 硬件"播完一块" → 本文件的 ISR 调 rt_audio_tx_complete()
 *                 └─ 框架 _audio_send_replay_frame() 把下一块 memcpy 进
 *                    ops->buffer_info() 报给它的那块缓冲（乒乓两半）
 *
 *   ⇒ 驱动只要做三件事：① 报告 ping-pong 缓冲的几何（buffer_info）；
 *     ② 按采样率把缓冲里的样本一个字节一个字节写进 LEDC 占空比（ISR）；
 *     ③ 每播完一半就 rt_audio_tx_complete() 让框架补下一半。
 *     `transmit` 留 NULL —— 框架直接往我们报的缓冲里写（和官方 STM32 SAI 驱动
 *     `bsp/stm32/stm32f429-atk-apollo/board/ports/audio/drv_sound.c` 一致）。
 *
 * 时钟：SYSTIMER TARGET1（TARGET0 被 1ms tick 占了），**周期模式**：
 *   period = 16MHz / fs（TARGET_CONF 的 PERIOD[25:0] + PERIOD_MODE）。
 *   16MHz 是 XTAL/2.5，跟 CPU 提频无关（所以音频速率不受 320MHz 影响）。
 *   ⚠️ 只能整除的速率是精确的（8k/16k/20k/32k…）；44.1k 会按 363 取整
 *      （实际 44077Hz，-0.05%，听不出来，但**别拿它做测速基准**）。
 *
 * 接线：GPIO20（J2-26 脚）→ 串一个 RC 低通（例如 1kΩ + 100nF，截止 ~1.6kHz，
 *   或者按你喇叭/功放输入阻抗再调）→ 小喇叭；GND 用 J2 的 33/34/37/38 脚。
 *   ⚠️ GPIO 直推喇叭声音很小（几 mA），要响就经功放（板载 NS4150B 的输入
 *      或者一个 PAM8302 之类的小功放板）。
 *
 * msh：
 *   sound          —— 看设备状态与缓冲几何
 *   tone <hz> [ms] —— 直接放一段正弦（不依赖文件，用来验证通路；省略 ms = 一直响）
 *   （放 WAV 见 app/wav_player.c 的 `wav_play`）
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/dev_audio.h>
#include <stdlib.h>
#include <math.h>
#include "soc/ledc_struct.h"        /* LEDC0 的字段名（冻结的 IDF 头）*/
#include "s31_regs.h"
#include "s31_iomux_table.h"        /* 焊盘 → IO_MUX 偏移查表 */

#define S31_AUDIO_PWM_GPIO      20          /* J2-26，本板上没别的用途 */
#define S31_AUDIO_LEDC_CH       0
#define S31_AUDIO_LEDC_TIMER    0
#define S31_AUDIO_CLIC_ID       (S31_CLIC_EXT_OFFSET + 3)   /* 19（16=tick 17=USJ 18=USB）*/

#define S31_AUDIO_DUTY_RES      8           /* 8 位 → 256 级；载波 = 源/256 */
#define S31_AUDIO_CARRIER_HZ    156250u     /* 40MHz(XTAL) / 256 */
#define S31_AUDIO_BLOCK_BYTES   1024        /* 每半块 1KB（8bit 单声道 16kHz = 64ms）*/
#define S31_AUDIO_TX_BYTES      (S31_AUDIO_BLOCK_BYTES * 2)

/* GPIO matrix / IO_MUX（和 bsp/drv_gpio.c、drv_spi.c 同一套地址）*/
#define S31_GPIO_BASE_          0x20583000u
#define S31_IO_MUX_BASE_        0x20582000u
#define S31_GPIO_FUNC_OUT_SEL(n) (S31_GPIO_BASE_ + 0xAF4u + 4u * (n))
#define S31_GPIO_ENABLE_W1TS_   (S31_GPIO_BASE_ + 0x38u)
#define S31_GPIO_ENABLE1_W1TS_  (S31_GPIO_BASE_ + 0x44u)
#define S31_GPIO_OUT_W1TS_      (S31_GPIO_BASE_ + 0x08u)   /* 软件 GPIO 输出置位/清位 */
#define S31_GPIO_OUT_W1TC_      (S31_GPIO_BASE_ + 0x0Cu)
#define S31_SIG_GPIO_OUT        256u    /* SIG_GPIO_OUT_IDX：OUT_SEL=256 → 走软件 GPIO_OUT */
#define S31_IOMUX_MCU_SEL_S     12
#define S31_IOMUX_FUN_IE        (1u << 9)
#define S31_IOMUX_FUN_PU        (1u << 8)
#define S31_IOMUX_FUN_PD        (1u << 7)

static inline void s31_audio_set_duty(rt_uint32_t duty);   /* 定义见下 */
rt_uint64_t s31_systimer_get_ticks(void);                  /* bsp/drv_systick.c：读 UNIT0 计数 */

/*===========================================================================
 * 引脚：焊盘 → GPIO 矩阵输出（跑到 LEDC 的信号上）
 *   ① IO_MUX：MCU_SEL = GPIO(1)，顺手清掉内部上下拉（LEDC 是推挽输出）
 *   ② GPIO_FUNCn_OUT_SEL_CFG = 信号号（LEDC0 通道0 = 126）
 *   ③ 打开该引脚的输出使能（oen_sel 用默认 0 = 由 GPIO_ENABLE 管）
 * ⚠️ 这里**不**碰 GPIO_OUT（那是"软件 GPIO 输出"那条路，OUT_SEL=256 才用它）
 *===========================================================================*/
static void s31_audio_pin_mux(int pad, rt_uint32_t sig_idx)
{
    rt_uint32_t v;

    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT || s31_iomux_off[pad] == 0xFFFFFFFFu) {
        rt_kprintf("[sound] GPIO%d 没有 IO_MUX，音频脚配不了\n", pad);
        return;
    }
    v  = S31_REG32(S31_IO_MUX_BASE_ + s31_iomux_off[pad]);
    v &= ~((0x7u << S31_IOMUX_MCU_SEL_S) | S31_IOMUX_FUN_IE | S31_IOMUX_FUN_PU | S31_IOMUX_FUN_PD);
    v |= (1u << S31_IOMUX_MCU_SEL_S);           /* MCU_SEL = 1 → 走 GPIO 矩阵 */
    S31_REG32(S31_IO_MUX_BASE_ + s31_iomux_off[pad]) = v;

    S31_REG32(S31_GPIO_FUNC_OUT_SEL(pad)) = sig_idx & 0x1FFu;

    if (pad < 32) {
        S31_REG32(S31_GPIO_ENABLE_W1TS_) = 1u << pad;
    } else {
        S31_REG32(S31_GPIO_ENABLE1_W1TS_) = 1u << (pad - 32);
    }
}

struct s31_pwm_audio
{
    struct rt_audio_device parent;
    /* 硬件 ping-pong 缓冲：框架往这里 memcpy，ISR 从这里取样本 */
    rt_uint8_t  tx[S31_AUDIO_TX_BYTES] __attribute__((aligned(8)));
    volatile rt_uint32_t pos;               /* ISR 当前播到哪 */
    volatile rt_uint8_t  running;
    volatile rt_uint32_t isr_cnt;
    rt_uint16_t samplerate;
    rt_uint16_t channels;
    rt_uint16_t samplebits;
    rt_uint8_t  vol_shift;                  /* 软件音量（右移位数，0 = 满音量）*/
};

static struct s31_pwm_audio s_audio;

/*===========================================================================
 * LEDC / GPIO 初始化
 *===========================================================================*/
static void s31_audio_ledc_init(void)
{
    rt_uint32_t v;

    /* 1) 时钟：APB 门控 + 复位脉冲 + 放开复位 + 时钟源 + 时钟使能
     *    （hp_sys_clkrst_reg.h:3275 的 ledc_ctrl0）*/
    S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0) |= S31_LEDC_CLK_APB_EN;
    S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0) |= S31_LEDC_CLK_RST_EN;
    S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0) &= ~S31_LEDC_CLK_RST_EN;
    S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0) |= S31_LEDC_CLK_FORCE_NORST;
    /* 时钟源**显式**选 XTAL(40MHz)：LEDC0_CLK_SRC_SEL 0=XTAL / 1=RC_FAST / 2=PLL_DIV
     * （编码出处 hal/esp32s31/ledc_ll.h 的 ledc_ll_set_slow_clk_sel；复位默认就是 0）*/
    v  = S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0);
    v &= ~S31_LEDC_CLK_SRC_SEL_M;
    v |=  (0u << S31_LEDC_CLK_SRC_SEL_S);
    S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0) = v;
    S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0) |= S31_LEDC_CLK_EN;

    /* 2) 强制打开寄存器时钟（空闲时也能写寄存器）*/
    LEDC0.conf.clk_en = 1;

    /* 3) 🚨 逐 timer / 逐通道**上电**（S31 的 LEDC 独有的两组位，复位默认是"断电"）
     *    真凶记录（2026-09-26）：少了这两条，寄存器**全都读回正确值**（div/duty_res/
     *    sig_out_en/计时器都没复位）、采样 ISR 也照跑（那是 SYSTIMER 的账），
     *    但定时器根本不计数、通道不输出 → **引脚是一条直线**（逻辑分析仪抓到的实况）。
     *    出处：hal/esp32s31/ledc_ll.h 的 ledc_ll_enable_timer_power / _channel_power，
     *    IDF 驱动在 ledc_timer_config()/ledc_channel_config() 里各开一次。*/
    LEDC0.timer_power_up_conf.val |= (1u << S31_AUDIO_LEDC_TIMER);
    LEDC0.ch_power_up_conf.val    |= (1u << S31_AUDIO_LEDC_CH);

    /* 4) 定时器：duty_res=8（256 级）、clk_div=1.0（低 8 位是小数部分）、先复位
     *    → para_up 让配置生效 → 放开复位 */
    ledc_timern_conf_reg_t tc = { 0 };
    tc.duty_res = S31_AUDIO_DUTY_RES;
    tc.clk_div  = (1u << 8);            /* 1.0：载波 = 源 / 2^8 */
    tc.rst      = 1;
    LEDC0.timer_group[0].timer[S31_AUDIO_LEDC_TIMER].conf.val = tc.val;
    tc.rst = 0;
    tc.para_up = 1;                     /* 同步 clk_div/duty_res 到定时器时钟域 */
    LEDC0.timer_group[0].timer[S31_AUDIO_LEDC_TIMER].conf.val = tc.val;

    /* 5) 通道：选 timer0、空闲电平低、使能输出；占空比先给中点（静音）*/
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].conf0.timer_sel = S31_AUDIO_LEDC_TIMER;
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].conf0.idle_lv   = 0;
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].conf0.sig_out_en = 1;
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].hpoint.hpoint    = 0;
    s31_audio_set_duty(128);            /* 中点 = "无信号"（约 VCC/2，RC 后是 0 电平）*/
}

/* 占空比写入（ISR 里也调 → 必须够快：三条寄存器写）
 * duty 字段是 25 位、**低 4 位是小数**（IDF ledc_ll_set_duty_int_part 就是 `duty << 4`）。
 *
 * 🚨 光写 duty_init **不生效**（2026-09-26 逻辑分析仪抓出来的第二条坑）：
 *    S31 的通道有影子寄存器，占空比要"锁存 + 通道参数更新"才落地 ——
 *    官方 `_ledc_update_duty()` 就是  sig_out_en=1 → duty_start=1 → conf0.para_up=1
 *    （= ledc_hal_set_sig_out_en + ledc_hal_set_duty_start + ledc_hal_ls_channel_update）。
 *    只写 duty 的现象：读回值全对、定时器也在数，但**输出恒定**（LA 是一条直线）。
 *    duty_start 是自清位（R/W/SC）、para_up 是 WT 位，所以每次写 1 都对。*/
static inline void s31_audio_set_duty(rt_uint32_t duty)
{
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].duty_init.duty  = duty << 4;
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].conf1.duty_start = 1;
    LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].conf0.para_up    = 1;
}

/*===========================================================================
 * 采样时钟：SYSTIMER TARGET1（周期模式）
 *===========================================================================*/
void s31_audio_clock_start(rt_uint16_t fs)
{
    rt_uint32_t period = (S31_SYSTIMER_HZ + fs / 2u) / fs;      /* 四舍五入 */
    rt_uint64_t now, first;

    /* 🚨 周期模式下**初始目标值必须给"当前计数 + period"**（2026-09-26 踩过）：
     *    周期模式只是在"上一次目标值"上累加 period，而比较是**精确匹配**——
     *    写 0 的话，第一个周期(0+period=1000)早就被计数器的当前值（几亿）甩在身后，
     *    于是**只中断一次就再也不来了**（现象：ISR 计数=1、占空比冻结、
     *    而 tone 的写入线程因为队列没人消费而永远阻塞）。
     *    为什么第一次还会来一下：比较器发现目标"已过期"会补一次。*/
    now   = s31_systimer_get_ticks();
    first = now + period;

    /* 先停：把目标先撤下来再改参数（避免改到一半就触发）*/
    S31_REG32(S31_SYSTIMER_CONF) &= ~S31_SYSTIMER_TARGET1_WORK_EN;
    S31_REG32(S31_SYSTIMER_INT_ENA) &= ~S31_SYSTIMER_T1_INT;

    S31_REG32(S31_SYSTIMER_TARGET1_HI)   = (rt_uint32_t)(first >> 32);
    S31_REG32(S31_SYSTIMER_TARGET1_LO)   = (rt_uint32_t)(first & 0xFFFFFFFFu);
    S31_REG32(S31_SYSTIMER_TARGET1_CONF) = (period & 0x03FFFFFFu) |
                                           S31_SYSTIMER_TARGET0_PERIOD_MODE |   /* bit30：周期模式 */
                                           0u;                                  /* bit31=0 → UNIT0 */
    S31_REG32(S31_SYSTIMER_COMP1_LOAD)   = 1;      /* 让目标值生效 */

    S31_REG32(S31_SYSTIMER_INT_CLR) = S31_SYSTIMER_T1_INT;
    S31_REG32(S31_SYSTIMER_INT_ENA) |= S31_SYSTIMER_T1_INT;
    S31_REG32(S31_SYSTIMER_CONF)    |= S31_SYSTIMER_TARGET1_WORK_EN;
}

void s31_audio_clock_stop(void)
{
    S31_REG32(S31_SYSTIMER_CONF)    &= ~S31_SYSTIMER_TARGET1_WORK_EN;
    S31_REG32(S31_SYSTIMER_INT_ENA) &= ~S31_SYSTIMER_T1_INT;
    S31_REG32(S31_SYSTIMER_INT_CLR)  = S31_SYSTIMER_T1_INT;
}

/*===========================================================================
 * 采样中断：一个 tick 出一个样本
 *===========================================================================*/
static void s31_audio_isr(int irq, void *param)
{
    rt_uint32_t p, frame;              /* p = **帧序号**（不是字节偏移！）*/
    rt_uint32_t v = 0;
    const rt_uint8_t *src = s_audio.tx;

    (void)irq;
    (void)param;

    S31_REG32(S31_SYSTIMER_INT_CLR) = S31_SYSTIMER_T1_INT;   /* 电平触发必须清 */
    if (!s_audio.running) {
        return;
    }
    s_audio.isr_cnt++;

    p = s_audio.pos;
    if (s_audio.samplebits == 16u) {
        if (s_audio.channels >= 2u) {   /* 立体声 16 位：左右平均（有符号 → 加偏置取高 8 位）*/
            rt_int32_t l = (rt_int16_t)(src[p * 4 + 0] | (src[p * 4 + 1] << 8));
            rt_int32_t r = (rt_int16_t)(src[p * 4 + 2] | (src[p * 4 + 3] << 8));
            v = (rt_uint32_t)(((l + r) / 2 + 32768) >> 8);
        } else {
            rt_int32_t l = (rt_int16_t)(src[p * 2 + 0] | (src[p * 2 + 1] << 8));
            v = (rt_uint32_t)((l + 32768) >> 8);
        }
    } else {
        if (s_audio.channels >= 2u) {
            v = ((rt_uint32_t)src[p * 2 + 0] + (rt_uint32_t)src[p * 2 + 1]) / 2u;
        } else {
            v = src[p];
        }
    }
    if (s_audio.vol_shift) {
        v >>= s_audio.vol_shift;
    }
    s31_audio_set_duty(v & 0xFFu);

    /* 一个中断 = 一帧。帧长（字节）= (位深/8) × 声道数。
     * 🚨 第一版这里把 p 同时当"样本序号"和"字节偏移"用（16 位时 +=2）→
     *    每块只放了该放的一半、后半段还读到缓冲区外面（16 位时 p*2 最大到 4092，
     *    而缓冲只有 2048）——听感是杂音。这里统一定义：**p 是帧序号**。*/
    frame = ((rt_uint32_t)(s_audio.samplebits / 8u)) * s_audio.channels;
    p++;
    if (p * frame >= S31_AUDIO_BLOCK_BYTES) {
        rt_audio_tx_complete(&s_audio.parent);      /* 前半放完 → 让框架补前半 */
        if (p * frame >= S31_AUDIO_TX_BYTES) {
            p = 0;
        }
    }
    s_audio.pos = p;
}

static void s31_audio_irq_install(void)
{
    rt_hw_interrupt_install(S31_AUDIO_CLIC_ID, s31_audio_isr, RT_NULL, "audio");

    S31_REG32(S31_INTR0_BASE + 4 * S31_ETS_SYSTIMER_TARGET1) = S31_AUDIO_CLIC_ID;
    S31_REG8(S31_CLIC_IP(S31_AUDIO_CLIC_ID))   = 0;
    S31_REG8(S31_CLIC_ATTR(S31_AUDIO_CLIC_ID)) = 0;
    S31_REG8(S31_CLIC_CTL(S31_AUDIO_CLIC_ID))  = S31_CLIC_CTL_PRIO(1);
    S31_REG8(S31_CLIC_IE(S31_AUDIO_CLIC_ID))   = 1;
}

/*===========================================================================
 * audio ops
 *===========================================================================*/
static rt_err_t s31_audio_init(struct rt_audio_device *audio)
{
    (void)audio;
    s31_audio_pin_mux(S31_AUDIO_PWM_GPIO, S31_LEDC_SIG_OUT_CH0);
    s31_audio_ledc_init();
    s31_audio_irq_install();

    s_audio.samplerate = 16000;
    s_audio.channels   = 1;
    s_audio.samplebits = 8;
    s_audio.pos        = 0;
    s_audio.running    = 0;

    rt_kprintf("[sound] LEDC PWM 音频就绪：GPIO%u，8bit 占空比 @%u Hz 载波"
               "（RC 低通后接喇叭/功放）\n",
               (unsigned)S31_AUDIO_PWM_GPIO, (unsigned)S31_AUDIO_CARRIER_HZ);
    return RT_EOK;
}

static rt_err_t s31_audio_start(struct rt_audio_device *audio, int stream)
{
    (void)audio;
    if (stream != AUDIO_STREAM_REPLAY) {
        return -RT_EINVAL;
    }
    s_audio.pos     = 0;
    s_audio.isr_cnt = 0;
    s_audio.running = 1;
    s31_audio_clock_start(s_audio.samplerate);
    return RT_EOK;
}

static rt_err_t s31_audio_stop(struct rt_audio_device *audio, int stream)
{
    (void)audio;
    if (stream != AUDIO_STREAM_REPLAY) {
        return -RT_EINVAL;
    }
    s_audio.running = 0;
    s31_audio_clock_stop();
    s31_audio_set_duty(128);            /* 回到中点 = 静音 */
    return RT_EOK;
}

static rt_err_t s31_audio_configure(struct rt_audio_device *audio, struct rt_audio_caps *caps)
{
    rt_err_t result = RT_EOK;

    (void)audio;
    switch (caps->sub_type) {
    case AUDIO_DSP_PARAM:
        s_audio.samplerate = caps->udata.config.samplerate;
        s_audio.channels   = caps->udata.config.channels;
        s_audio.samplebits = caps->udata.config.samplebits;
        break;
    case AUDIO_DSP_SAMPLERATE:
        s_audio.samplerate = caps->udata.config.samplerate;
        break;
    case AUDIO_DSP_CHANNELS:
        s_audio.channels = caps->udata.config.channels;
        break;
    case AUDIO_DSP_SAMPLEBITS:
        s_audio.samplebits = caps->udata.config.samplebits;
        break;
    default:
        result = -RT_ERROR;
        break;
    }

    /* 只支持 8/16 位、1/2 声道（PWM 的天然形状）*/
    if (s_audio.samplebits != 8u && s_audio.samplebits != 16u) {
        result = -RT_EINVAL;
    }
    if (s_audio.channels < 1u || s_audio.channels > 2u) {
        result = -RT_EINVAL;
    }
    if (s_audio.samplerate < 4000u || s_audio.samplerate > 48000u) {
        result = -RT_EINVAL;
    }

    /* 正在放的时候改采样率：重启采样时钟（不清缓冲，会有一点点杂音）*/
    if (result == RT_EOK && s_audio.running) {
        s31_audio_clock_start(s_audio.samplerate);
    }
    return result;
}

static rt_err_t s31_audio_getcaps(struct rt_audio_device *audio, struct rt_audio_caps *caps)
{
    rt_err_t result = RT_EOK;

    (void)audio;
    switch (caps->main_type) {
    case AUDIO_TYPE_QUERY:
        caps->udata.mask = AUDIO_TYPE_OUTPUT;
        break;
    case AUDIO_TYPE_OUTPUT:
        switch (caps->sub_type) {
        case AUDIO_DSP_PARAM:
            caps->udata.config.samplerate = s_audio.samplerate;
            caps->udata.config.channels   = s_audio.channels;
            caps->udata.config.samplebits = s_audio.samplebits;
            break;
        case AUDIO_DSP_SAMPLERATE: caps->udata.config.samplerate = s_audio.samplerate; break;
        case AUDIO_DSP_CHANNELS:   caps->udata.config.channels   = s_audio.channels;   break;
        case AUDIO_DSP_SAMPLEBITS: caps->udata.config.samplebits = s_audio.samplebits; break;
        default: result = -RT_ERROR; break;
        }
        break;
    default:
        result = -RT_ERROR;
        break;
    }
    return result;
}

/* ping-pong 缓冲几何：框架按这个往 buffer 里填（block_size 一半一半地填）*/
static void s31_audio_buffer_info(struct rt_audio_device *audio, struct rt_audio_buf_info *info)
{
    (void)audio;
    info->buffer      = s_audio.tx;
    info->total_size  = S31_AUDIO_TX_BYTES;
    info->block_size  = S31_AUDIO_BLOCK_BYTES;
    info->block_count = 2;
}

static struct rt_audio_ops s31_audio_ops =
{
    .getcaps     = s31_audio_getcaps,
    .configure   = s31_audio_configure,
    .init        = s31_audio_init,
    .start       = s31_audio_start,
    .stop        = s31_audio_stop,
    .transmit    = RT_NULL,             /* 框架直接写我们报的缓冲 */
    .buffer_info = s31_audio_buffer_info,
};

/*===========================================================================
 * sound -g：软件 GPIO 方波自检（把 LEDC 摘出去，单独验"焊盘 → 探头"这条路）
 *---------------------------------------------------------------------------
 * 为什么要有它：LEDC 链路出故障时现象是"寄存器全对、引脚不动"（见上电位那条坑），
 * 光看固件分不清"芯片真没输出"还是"探头没夹好/夹错脚"。
 * 这条把音频脚从 LEDC 信号上摘下来接到**软件 GPIO_OUT**，用 SYSTIMER 定一个
 * ~1 kHz 方波（示波器/LA 一眼可辨），跑完再把路由还回 LEDC。
 *===========================================================================*/
#define S31_GPIO_TEST_HZ   1000u

static void s31_audio_gpio_selftest(rt_uint32_t ms)
{
    const int pad = S31_AUDIO_PWM_GPIO;
    rt_uint32_t half = S31_SYSTIMER_HZ / (2u * S31_GPIO_TEST_HZ);   /* 半周期（tick）*/
    rt_uint32_t edges = 0;
    rt_uint64_t t0, end, next, now;
    int lv = 0;

    if (s_audio.running) {
        rt_kprintf("[sound] 正在放音，先停下再自检（tone 0 / 等播放结束）\n");
        return;
    }

    /* 摘掉 LEDC 信号，接上软件 GPIO_OUT；输出使能已经在 pin_mux 里开着 */
    S31_REG32(S31_GPIO_FUNC_OUT_SEL(pad)) = S31_SIG_GPIO_OUT;
    S31_REG32(S31_GPIO_ENABLE_W1TS_)      = 1u << pad;

    rt_kprintf("[sound] 软件 GPIO 方波自检：GPIO%d，%u Hz，%u ms"
               "（探头接这个脚，触发用上升沿）\n",
               pad, (unsigned)S31_GPIO_TEST_HZ, (unsigned)ms);

    t0   = s31_systimer_get_ticks();
    end  = t0 + (rt_uint64_t)S31_SYSTIMER_HZ * ms / 1000u;
    next = t0;
    while (1) {
        now = s31_systimer_get_ticks();
        if (now >= end) {
            break;
        }
        if (now >= next) {
            next += half;
            lv = !lv;
            if (lv) {
                S31_REG32(S31_GPIO_OUT_W1TS_) = 1u << pad;
            } else {
                S31_REG32(S31_GPIO_OUT_W1TC_) = 1u << pad;
            }
            edges++;
        }
    }
    S31_REG32(S31_GPIO_OUT_W1TC_) = 1u << pad;      /* 收尾拉低 */

    /* 路由还回 LEDC，并回到中点静音 */
    S31_REG32(S31_GPIO_FUNC_OUT_SEL(pad)) = S31_LEDC_SIG_OUT_CH0;
    s31_audio_set_duty(128);

    rt_kprintf("[sound] 自检结束：翻了 %u 次（约 %u 个方波周期），路由已还回 LEDC\n",
               (unsigned)edges, (unsigned)(edges / 2u));
}

/*===========================================================================
 * msh：sound / tone
 *===========================================================================*/
static void sound(int argc, char **argv)
{
    if ((argc >= 2) && (rt_strcmp(argv[1], "-g") == 0)) {
        s31_audio_gpio_selftest((argc >= 3) ? (rt_uint32_t)atoi(argv[2]) : 3000u);
        return;
    }

    if ((argc >= 2) && (rt_strcmp(argv[1], "-m") == 0)) {
        /* 自检：量 1 秒里 ISR 到底跑了多少次（= 实际采样率），
         * 顺便扫一眼占空比寄存器有没有在动（能证明 ISR 真的在改它）。
         * 为什么需要它：没有示波器/LA 的时候，"有声没声"只能靠耳朵，
         * 而"采样率对不对"这条用软件就能量准（SYSTIMER 是按 16MHz 数的）。*/
        rt_uint32_t c0, c1, dmin = 0xFFFFFFFFu, dmax = 0, dsum = 0, n = 0;
        rt_tick_t t0;
        int i;

        c0 = s_audio.isr_cnt;
        t0 = rt_tick_get();
        for (i = 0; i < 1000; i++) {
            rt_uint32_t d = LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].duty_init.duty >> 4;
            if (d < dmin) { dmin = d; }
            if (d > dmax) { dmax = d; }
            dsum += d;
            n++;
            rt_thread_mdelay(1);
        }
        c1 = s_audio.isr_cnt;
        rt_kprintf("[sound] %u ms 实测：ISR %u 次 → 采样率 ≈ %u Hz（期望 %u）%s\n",
                   (unsigned)((rt_tick_get() - t0) * 1000u / RT_TICK_PER_SECOND),
                   (unsigned)(c1 - c0),
                   (unsigned)((c1 - c0) * RT_TICK_PER_SECOND / (rt_tick_get() - t0)),
                   (unsigned)s_audio.samplerate,
                   s_audio.running ? "" : "（当前没在放：ISR 计数不会涨，正常）");
        rt_kprintf("[sound] 占空比抽样 %u 次：min=%u max=%u avg=%u（都在 0..255 里说明转换对了）\n",
                   (unsigned)n, (unsigned)(dmin == 0xFFFFFFFFu ? 0 : dmin),
                   (unsigned)dmax, (unsigned)(n ? dsum / n : 0u));
        return;
    }

    rt_kprintf("sound0（LEDC PWM 放音，走 RT-Thread audio 框架）\n");
    rt_kprintf("  GPIO%u  载波 %u Hz  占空比 %u 位（%u 级）\n",
               (unsigned)S31_AUDIO_PWM_GPIO, (unsigned)S31_AUDIO_CARRIER_HZ,
               (unsigned)S31_AUDIO_DUTY_RES, 1u << S31_AUDIO_DUTY_RES);
    rt_kprintf("  当前配置 : %u Hz / %u 声道 / %u 位%s\n",
               (unsigned)s_audio.samplerate, (unsigned)s_audio.channels,
               (unsigned)s_audio.samplebits, s_audio.running ? "（正在放）" : "");
    rt_kprintf("  乒乓缓冲 : %u 字节 × 2（每块 %u ms @%uHz/8bit/mono）\n",
               (unsigned)S31_AUDIO_BLOCK_BYTES,
               (unsigned)(S31_AUDIO_BLOCK_BYTES * 1000u / s_audio.samplerate),
               (unsigned)s_audio.samplerate);
    rt_kprintf("  已出样本 : %u（ISR 计数）\n", (unsigned)s_audio.isr_cnt);
    rt_kprintf("  LEDC     : clk_ctrl0=0x%08x timer0_conf=0x%08x chn0_conf0=0x%08x duty=0x%08x\n",
               (unsigned)S31_REG32(S31_HP_SYS_CLKRST_LEDC_CTRL0),
               (unsigned)LEDC0.timer_group[0].timer[S31_AUDIO_LEDC_TIMER].conf.val,
               (unsigned)LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].conf0.val,
               (unsigned)LEDC0.channel_group[0].channel[S31_AUDIO_LEDC_CH].duty_init.val);
    rt_kprintf("  上电位   : timer_power_up=0x%08x ch_power_up=0x%08x"
               "（S31 特有：必须非 0，否则定时器不计数、引脚不动）\n",
               (unsigned)LEDC0.timer_power_up_conf.val,
               (unsigned)LEDC0.ch_power_up_conf.val);
    rt_kprintf("  用法     : tone <hz> [ms]   放正弦（验证通路）\n");
    rt_kprintf("             wav_play <文件>  放 WAV（见 wav_play -h）\n");
    rt_kprintf("             sound -m         量 1 秒的实际采样率 + 占空比范围\n");
    rt_kprintf("             sound -g [ms]    软件 GPIO 方波自检（验引脚/探头，默认 3s）\n");
}
MSH_CMD_EXPORT(sound, show PWM audio device (sound0) status);

/*===========================================================================
 * tone：放一段正弦（验证"PWM → RC → 喇叭"这条通路，不依赖文件系统）
 *
 * 🚨 第一版在这里栽过：`tone <hz>`（不带 ms）直接在 shell 线程里跑一个
 *    "生成到天荒地老"的循环，每点都调 sinf()，而且 n 越滚越大 → 参数巨大
 *    → 落进 libm 的慢路径(__kernel_rem_pio2f) → **shell 再也回不来**
 *    （现象极像"板子卡死"，JTAG 采 PC 才发现是在 libm 里磨）。
 *    现在：① 正弦用**运行时建的 256 项查表**，循环里只有一次查表；
 *          ② 缓冲**预先算好 1 秒**，之后只是反复喂同一段；
 *          ③ 不限时（ms 省略）时丢给一个后台线程，shell 立刻返回。
 *===========================================================================*/
#define TONE_FS        16000u
#define TONE_BUF_BYTES (TONE_FS)            /* 1 秒 = 16000 字节（8bit 单声道）*/

static rt_int8_t s_sin8[256];
static rt_uint8_t s_tone_buf[TONE_BUF_BYTES];
static volatile int s_tone_stop;
static rt_thread_t s_tone_thread;

static void s31_tone_build(void)
{
    int i;
    const float two_pi = 6.2831853072f;     /* 自己写常量：newlib 的 M_PI 要 _USE_MATH_DEFINES */
    for (i = 0; i < 256; i++) {
        s_sin8[i] = (rt_int8_t)(127.0f * sinf(two_pi * (float)i / 256.0f));
    }
}

static void s31_tone_fill(rt_uint32_t hz)
{
    rt_uint32_t phase = 0;
    rt_uint32_t inc = (hz * 256u) / TONE_FS;        /* 每采样相位增量（8.8 定点）*/
    rt_uint32_t i;

    for (i = 0; i < TONE_BUF_BYTES; i++) {
        rt_int32_t v = s_sin8[(phase >> 8) & 0xFFu];
        s_tone_buf[i] = (rt_uint8_t)(128 + (v * 100) / 127);
        phase += inc;
    }
}

static rt_device_t s31_tone_open(void)
{
    rt_device_t dev = rt_device_find("sound0");
    struct rt_audio_caps caps;

    if (dev == RT_NULL) {
        rt_kprintf("[tone] 找不到 sound0\n");
        return RT_NULL;
    }
    if (rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY) != RT_EOK) {
        rt_kprintf("[tone] 打开 sound0 失败\n");
        return RT_NULL;
    }
    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = TONE_FS;
    caps.udata.config.channels   = 1;
    caps.udata.config.samplebits = 8;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);
    return dev;
}

static void s31_tone_thread(void *param)
{
    rt_device_t dev = (rt_device_t)param;

    while (!s_tone_stop) {
        rt_device_write(dev, 0, s_tone_buf, sizeof(s_tone_buf));
    }
    rt_device_close(dev);
    rt_kprintf("[tone] 停\n");
}

static void tone(int argc, char **argv)
{
    rt_uint32_t hz;
    rt_int32_t  ms;

    if (argc < 2) {
        rt_kprintf("用法: tone <hz> [ms]（省略 ms = 一直响；`tone 0` 停）\n");
        return;
    }
    hz = (rt_uint32_t)atoi(argv[1]);
    ms = (argc >= 3) ? atoi(argv[2]) : -1;

    /* --- 停 --- */
    if (hz == 0u || s_tone_thread != RT_NULL) {
        s_tone_stop = 1;
        if (s_tone_thread != RT_NULL) {
            int guard = 200;
            while (s_tone_thread != RT_NULL && guard--) {
                rt_thread_mdelay(10);           /* 等后台线程自己 close 掉设备 */
            }
            if (s_tone_thread != RT_NULL) {     /* 兜底：线程没退就强制 */
                rt_thread_delete(s_tone_thread);
                s_tone_thread = RT_NULL;
            }
        }
        if (hz == 0u) {
            return;
        }
    }

    s31_tone_build();
    s31_tone_fill(hz);

    rt_device_t dev = s31_tone_open();
    if (dev == RT_NULL) {
        return;
    }

    if (ms < 0) {
        /* 不限时：交给后台线程反复喂，shell 立刻回来 */
        s_tone_stop = 0;
        s_tone_thread = rt_thread_create("tone", s31_tone_thread, dev, 1024, 22, 10);
        if (s_tone_thread == RT_NULL) {
            rt_kprintf("[tone] 线程建不起来\n");
            rt_device_close(dev);
            return;
        }
        rt_thread_startup(s_tone_thread);
        rt_kprintf("[tone] 一直响 %u Hz（`tone 0` 停）\n", (unsigned)hz);
        return;
    }

    /* 限时：同步喂够 ms 毫秒（1 秒缓冲反复喂，余数截到缓冲边界）*/
    {
        rt_uint32_t left = (rt_uint32_t)ms * (TONE_FS / 1000u);
        while (left > 0u) {
            rt_uint32_t n = (left > sizeof(s_tone_buf)) ? sizeof(s_tone_buf) : left;
            rt_device_write(dev, 0, s_tone_buf, n);
            left -= n;
        }
    }
    rt_device_close(dev);
    rt_kprintf("[tone] 放完 %d ms @%u Hz\n", ms, (unsigned)hz);
}
MSH_CMD_EXPORT(tone, play a sine tone via sound0: tone <hz> [ms]);

static int s31_audio_pwm_init(void)
{
    s_audio.parent.ops = &s31_audio_ops;
    rt_audio_register(&s_audio.parent, "sound0", RT_DEVICE_FLAG_WRONLY, &s_audio);
    return RT_EOK;
}
INIT_DEVICE_EXPORT(s31_audio_pwm_init);
