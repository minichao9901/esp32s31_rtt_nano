/*===========================================================================
 * drv_i2c.c -- I2C 主机驱动（挂 RT-Thread 的 I2C 设备框架）
 *
 * 接上框架之后就能用标准 API：
 *      struct rt_i2c_bus_device *bus = rt_i2c_bus_device_find("i2c0");
 *      rt_i2c_transfer(bus, msgs, n);
 *
 * 寄存器来源（全部核对 IDF 6.1 的 soc/esp32s31 与 esp_hal_i2c/esp32s31）：
 *   基址 I2C0 = 0x20385000、I2C1 = 0x20386000
 *   +0x00 SCL_LOW_PERIOD  +0x04 CTR  +0x08 SR  +0x0c TO  +0x10 SLAVE_ADDR
 *   +0x1c DATA(FIFO,32B)  +0x20 INT_RAW +0x24 INT_CLR +0x28 INT_ENA
 *   +0x30 SDA_HOLD  +0x34 SDA_SAMPLE  +0x38 SCL_HIGH_PERIOD([8:0]=high,
 *                                                        [15:9]=wait_high)
 *   +0x40 SCL_START_HOLD +0x44 SCL_RSTART_SETUP +0x48 SCL_STOP_HOLD
 *   +0x4c SCL_STOP_SETUP +0x50 FILTER_CFG +0x58 COMD0 … +0x74 COMD7
 *
 * 🚨 三个"注释骗人"的地方（都实测/查源码确认过）：
 *   1. i2c_reg.h 里 COMD 的 op_code 注释是错的（写成 0=RSTART/2=READ/3=STOP）。
 *      正确值看 esp_hal_i2c/esp32s31/include/hal/i2c_ll.h:54-58：
 *          RSTART=6  WRITE=1  STOP=2  READ=3  END=4
 *   2. I2C_CTR 里**没有**时钟分频位（老芯片有）→ 分频在 HP_SYS_CLKRST 的
 *      CLK_DIV_NUM[12:5]；而且 **APB_CLK_EN(bit0) 与 CLK_EN(bit4) 复位默认是 0**，
 *      不打开就一点波形都没有。
 *   3. 地址字节是**当数据塞进 FIFO** 的（IDF 主机也这么做：
 *      i2c_master.c:194 `hw_cmd.byte_num = data_fill + *address_fill`），
 *      不是靠 SLAVE_ADDR 寄存器自动补。
 *   4. 开漏 = CTR 的 SDA/SCL_FORCE_OUT **写 0**（i2c_ll.h:897 `= !enable_od`）。
 *   5. 配完必须写 CTR.CONF_UPGATE(bit11) 才生效。
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "s31_regs.h"
#include "s31_iomux_table.h"

#define S31_I2C0_BASE      0x20385000u
#define S31_I2C1_BASE      0x20386000u

/* 寄存器偏移 */
#define I2C_SCL_LOW_PERIOD  0x00u
#define I2C_CTR             0x04u
#define I2C_SR              0x08u
#define I2C_TO              0x0cu
#define I2C_DATA            0x1cu
#define I2C_INT_RAW         0x20u
#define I2C_INT_CLR         0x24u
#define I2C_INT_ENA         0x28u
#define I2C_SDA_HOLD        0x30u
#define I2C_SDA_SAMPLE      0x34u
#define I2C_SCL_HIGH_PERIOD 0x38u
#define I2C_SCL_START_HOLD  0x40u
#define I2C_SCL_RSTART_SETUP 0x44u
#define I2C_SCL_STOP_HOLD   0x48u
#define I2C_SCL_STOP_SETUP  0x4cu
#define I2C_FIFO_CONF       0x18u
#define I2C_COMD(i)         (0x58u + 4u * (i))

/* FIFO_CONF：bit12 RX_FIFO_RST、bit13 TX_FIFO_RST（脉冲写 1 再写 0） */
#define I2C_RX_FIFO_RST      (1u << 12)
#define I2C_TX_FIFO_RST      (1u << 13)

/* CTR 位 */
#define I2C_CTR_SDA_FORCE_OUT (1u << 0)   /* 0 = 开漏（我们要的） */
#define I2C_CTR_SCL_FORCE_OUT (1u << 1)   /* 0 = 开漏 */
#define I2C_CTR_MS_MODE       (1u << 4)
#define I2C_CTR_TRANS_START   (1u << 5)
#define I2C_CTR_CLK_EN        (1u << 8)
#define I2C_CTR_CONF_UPGATE   (1u << 11)  /* 配完要写这位才生效 */

/* 中断位 */
#define I2C_INT_END_DETECT    (1u << 3)
#define I2C_INT_ARB_LOST      (1u << 5)
#define I2C_INT_TRANS_DONE    (1u << 7)
#define I2C_INT_TIME_OUT      (1u << 8)
#define I2C_INT_NACK          (1u << 10)

/* SR 位 */
#define I2C_SR_BUS_BUSY       (1u << 4)

/* 命令字 op_code（i2c_ll.h:54-58，别抄 i2c_reg.h 的注释） */
#define I2C_OP_RSTART   6u
#define I2C_OP_WRITE    1u
#define I2C_OP_STOP     2u
#define I2C_OP_READ     3u
#define I2C_OP_END      4u

/* 时钟门控寄存器（HP_SYS_CLKRST） */
#define CLKRST_I2C0_REG   (S31_HP_SYS_CLKRST_BASE + 0xDCu)
#define CLKRST_I2C1_REG   (S31_HP_SYS_CLKRST_BASE + 0xE0u)
#define CLKRST_APB_CLK_EN (1u << 0)
#define CLKRST_RST_EN     (1u << 1)
#define CLKRST_CLK_SRC_SEL (1u << 3)      /* 0 = XTAL, 1 = RC_FAST */
#define CLKRST_CLK_EN     (1u << 4)
#define CLKRST_CLK_DIV_S  5               /* CLK_DIV_NUM[12:5] */

/* GPIO matrix */
#define S31_GPIO_BASE        0x20583000u
#define GPIO_FUNC_OUT_SEL(n) (S31_GPIO_BASE + 0xAF4u + 4u * (n))  /* n = 焊盘号 */
#define GPIO_FUNC_IN_SEL(m)  (S31_GPIO_BASE + 0x2F4u + 4u * (m))  /* m = 输入信号号 */
#define GPIO_IN_SEL_MATRIX   (1u << 9)    /* 🚨 bit9=1 才走 matrix，否则信号进不来 */
#define I2C0_SCL_SIG        68u
#define I2C0_SDA_SIG        69u
#define I2C1_SCL_SIG        70u
#define I2C1_SDA_SIG        71u

/* IO_MUX（与 drv_gpio.c 同一套位） */
#define S31_IO_MUX_BASE     0x20582000u
#define IOMUX_MCU_SEL_S     12
#define IOMUX_FUN_IE        (1u << 9)
#define IOMUX_FUN_PU        (1u << 8)
#define IOMUX_FUN_PD        (1u << 7)
#define IOMUX_PIN_FUNC_GPIO 1u

/* GPIO 引脚配置寄存器（PAD_DRIVER=bit2，1 = 开漏） */
#define GPIO_PIN_REG(n)     (S31_GPIO_BASE + 0xF4u + 4u * (n))
#define GPIO_PIN_OD         (1u << 2)
#define GPIO_IN_REG_LOW     (S31_GPIO_BASE + 0x64u)
#define GPIO_IN_REG_HIGH    (S31_GPIO_BASE + 0x68u)

struct s31_i2c
{
    struct rt_i2c_bus_device parent;
    rt_uint32_t base;
};

/* 与 CPU 频率无关的时间基准（16MHz 固定），用于"结算窗口"这类跟真实时间有关的等待 */
extern rt_uint64_t s31_systimer_get_ticks(void);

static struct s31_i2c s_i2c0;
static struct s31_i2c s_i2c1;

static inline rt_uint32_t i2c_rd(struct s31_i2c *i2c, rt_uint32_t off)
{
    return S31_REG32(i2c->base + off);
}

static inline void i2c_wr(struct s31_i2c *i2c, rt_uint32_t off, rt_uint32_t v)
{
    S31_REG32(i2c->base + off) = v;
}

/* 把某个焊盘接到 I2C 外设的 SCL/SDA（走 GPIO matrix，开内部上拉） */
static void i2c_set_pin(rt_base_t pad, rt_uint32_t sig)
{
    rt_uint32_t off;
    rt_uint32_t v;

    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    off = s31_iomux_off[pad];
    if (off == 0xFFFFFFFFu) {
        return;
    }

    /* IO_MUX：GPIO 功能 + 输入使能 + 内部上拉
     * （本板 SDA/SCL 上没有外部上拉，AGENTS §4.2 记过：不开内部上拉一个设备都扫不到）*/
    v  = S31_REG32(S31_IO_MUX_BASE + off);
    v &= ~((0x7u << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE | IOMUX_FUN_PU | IOMUX_FUN_PD);
    v |= (IOMUX_PIN_FUNC_GPIO << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE | IOMUX_FUN_PU;
    S31_REG32(S31_IO_MUX_BASE + off) = v;

    /* 🚨 焊盘必须设成**开漏**（IDF 里就是 GPIO_MODE_INPUT_OUTPUT_OD）：
     * I2C_CTR 的 FORCE_OUT=0 只管外设内部，焊盘若还是推挽，输出恒 0 会把线拉死，
     * 主设备读 ACK 永远读到 0 —— 症状是"每个地址都扫到设备"（实测踩过：63 个全应答）。*/
    S31_REG32(GPIO_PIN_REG(pad)) |= GPIO_PIN_OD;

    /* 输出：焊盘 ← 外设信号 */
    S31_REG32(GPIO_FUNC_OUT_SEL(pad)) = sig;
    /* 输入：外设信号 ← 焊盘（低 8 位是焊盘号，bit9 必须置 1 走 matrix） */
    S31_REG32(GPIO_FUNC_IN_SEL(sig)) = (rt_uint32_t)pad | GPIO_IN_SEL_MATRIX;
}

static inline rt_uint32_t i2c_cmd(rt_uint32_t op, rt_uint32_t ack_en,
                                  rt_uint32_t ack_exp, rt_uint32_t ack_val,
                                  rt_uint32_t bytes)
{
    return (op << 11) | (ack_val << 10) | (ack_exp << 9) | (ack_en << 8) | (bytes & 0xFFu);
}

/* 🚨 每次传输前必须复位 FIFO（IDF 的 i2c_ll_txfifo_rst/rxfifo_rst 就是这么干的）。
 * 不复位的话：FIFO 写指针可能被 ROM/上一轮留在脏位置 —— 数据"写进去了"但硬件
 * 读不到，WRITE 命令实际一个字节都没发，总线上没有波形 → 所有地址一律 NACK。
 * （实测踩过：地址字节一直留在 TX FIFO 里，63 个地址全"有设备"或全"没设备"。）*/
static void i2c_fifo_reset(struct s31_i2c *i2c)
{
    i2c_wr(i2c, I2C_FIFO_CONF, I2C_TX_FIFO_RST | I2C_RX_FIFO_RST);
    i2c_wr(i2c, I2C_FIFO_CONF, 0);
}

/* 等一次硬件事务结束：返回 0 成功；负值为错误
 *
 * 判定方式照 IDF 的 i2c_ll_master_get_event（i2c_ll.h:1079-1097）：
 *   ① 先等 TRANS_START 自清 —— 这才是"事务真的跑完了"（只看到 TRANS_DONE 就判，
 *      可能 NACK 位还没落地，于是"没设备的地址"也被当成找到了 —— 实测踩过：
 *      扫描报 66 个设备全应答）
 *   ② 再读一次中断标志，按优先级判：ARB_LOST > NACK > TIME_OUT > TRANS_DONE
 *
 * ⚠️ 别用 SR.RESP_REC 判 ACK：寄存器头注释说 0=ACK/1=NACK，实测正好相反
 *    （板载 ES8311 明明应答了却读到 1）。
 */
/* 等一次硬件事务结束：返回 0 成功；负值为错误
 *
 * 三个坑（都实测踩过，别改回去）：
 * ① **必须只看"本次新出现"的标志位**：INT_CLR 清不掉 TRANS_DONE 之类的残留
 *    （实测下一次探测前 INT_RAW 还留着 0x80），直接累积就会把上一轮的 NACK
 *    算到这一轮头上 —— 症状是"背靠背扫描时假阳性/假阴性交替出现"。
 * ② **结算窗口不能按循环次数计**：CPU 从 40MHz 换到 320MHz 后同样次数只等 1/8 时间。
 *    这里用 SYSTIMER（固定 16MHz）计 300us，且**不早退**。
 * ③ ACK/NACK 除了中断位，再看一眼 `SR.RESP_REC`：S31 上它是
 *    **1 = 从机应答了 / 0 = 没人应答**（寄存器头注释写的 0=ACK/1=NACK 是反的，
 *    实测 ES8311 应答时读到 1）。两个口径一起用，互为保险。 */
static rt_err_t i2c_wait_done(struct s31_i2c *i2c, rt_uint32_t before, int check_ack)
{
    volatile rt_uint32_t guard = 2000000u;
    rt_uint64_t t0;
    rt_uint32_t acc = 0;
    rt_uint32_t raw, sr;

    while (--guard) {
        raw = i2c_rd(i2c, I2C_INT_RAW) & ~before;
        acc |= raw;
        if (acc & (I2C_INT_ARB_LOST | I2C_INT_NACK | I2C_INT_TIME_OUT | I2C_INT_TRANS_DONE)) {
            break;
        }
    }

    t0 = s31_systimer_get_ticks();
    while ((s31_systimer_get_ticks() - t0) < 300u * 16u) {      /* 300us 固定窗口 */
        acc |= i2c_rd(i2c, I2C_INT_RAW) & ~before;
    }

    sr = i2c_rd(i2c, I2C_SR);
    i2c_wr(i2c, I2C_INT_CLR, 0xFFFFu);

    if (guard == 0) {
        return -RT_ETIMEOUT;
    }
    if (acc & I2C_INT_ARB_LOST) {
        return -RT_ERROR;
    }
    if (acc & I2C_INT_NACK) {
        return -RT_ERROR;
    }
    if (acc & I2C_INT_TIME_OUT) {
        return -RT_ETIMEOUT;
    }
    if (check_ack && !(sr & 1u)) {       /* RESP_REC=0 → 这轮没有从机应答 */
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_ssize_t s31_i2c_xfer(struct rt_i2c_bus_device *bus, struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    struct s31_i2c *i2c = (struct s31_i2c *)bus->priv;
    rt_uint32_t i;
    rt_err_t err = RT_EOK;

    if (num == 0) {
        return 0;
    }

    i2c_wr(i2c, I2C_INT_CLR, 0xFFFFu);
    /* 清不掉的残留先记下来，后面只看"本次新出现"的位（见 i2c_wait_done 注释①） */
    rt_uint32_t flag_baseline = i2c_rd(i2c, I2C_INT_RAW);

    for (i = 0; i < num; i++) {
        struct rt_i2c_msg *m = &msgs[i];
        int next_no_start = (i + 1 < num) && (msgs[i + 1].flags & RT_I2C_NO_START);
        int ignore_nack  = (m->flags & RT_I2C_IGNORE_NACK) ? 1 : 0;
        rt_uint32_t cmd = 0;
        rt_uint32_t j;

        i2c_fifo_reset(i2c);         /* 数据进 FIFO 之前先清指针 */

        if (!(m->flags & RT_I2C_NO_START)) {
            i2c_wr(i2c, I2C_COMD(cmd++), i2c_cmd(I2C_OP_RSTART, 0, 0, 0, 0));
        }

        if (m->flags & RT_I2C_RD) {
            /* 地址字节（R=1）当数据发 */
            i2c_wr(i2c, I2C_DATA, ((rt_uint32_t)m->addr << 1) | 1u);
            i2c_wr(i2c, I2C_COMD(cmd++), i2c_cmd(I2C_OP_WRITE, 1, 0, 0, 1));

            /* 前 n-1 个字节回 ACK，最后一个回 NACK（ack_val=1）告诉从机读完了 */
            if (m->len > 1) {
                i2c_wr(i2c, I2C_COMD(cmd++), i2c_cmd(I2C_OP_READ, 1, 0, 0, m->len - 1));
            }
            i2c_wr(i2c, I2C_COMD(cmd++), i2c_cmd(I2C_OP_READ, 1, 0, 1, 1));
        } else {
            /* 地址字节（W=0）+ 数据，一起塞 FIFO，byte_num 把它们算在一起 */
            i2c_wr(i2c, I2C_DATA, ((rt_uint32_t)m->addr << 1));
            for (j = 0; j < m->len; j++) {
                i2c_wr(i2c, I2C_DATA, m->buf[j]);
            }
            i2c_wr(i2c, I2C_COMD(cmd++),
                   i2c_cmd(I2C_OP_WRITE, ignore_nack ? 0u : 1u, 0, 0, m->len + 1));
        }

        /* 后面还有"不发 START 的续传"就不发 STOP，用 END 收尾 */
        if (next_no_start) {
            i2c_wr(i2c, I2C_COMD(cmd++), i2c_cmd(I2C_OP_END, 0, 0, 0, 0));
        } else {
            i2c_wr(i2c, I2C_COMD(cmd++), i2c_cmd(I2C_OP_STOP, 0, 0, 0, 0));
        }

        /* 配完命令要写 CONF_UPGATE 才生效，然后 TRANS_START */
        i2c_wr(i2c, I2C_CTR, i2c_rd(i2c, I2C_CTR) | I2C_CTR_CONF_UPGATE);
        i2c_wr(i2c, I2C_CTR, i2c_rd(i2c, I2C_CTR) | I2C_CTR_TRANS_START);

        /* 写事务看 RESP_REC（NACK = 没有设备）；读事务最后一字节是我们自己回的 NACK，不能看 */
        err = i2c_wait_done(i2c, flag_baseline, (m->flags & RT_I2C_RD) ? 0 : 1);
        if (err != RT_EOK) {
            break;
        }

        if (m->flags & RT_I2C_RD) {
            for (j = 0; j < m->len; j++) {
                m->buf[j] = (rt_uint8_t)(i2c_rd(i2c, I2C_DATA) & 0xFFu);
            }
        }
    }

    return (err == RT_EOK) ? (rt_ssize_t)num : (rt_ssize_t)err;
}

static rt_err_t s31_i2c_control(struct rt_i2c_bus_device *bus, int cmd, void *args)
{
    (void)bus; (void)cmd; (void)args;
    return RT_EOK;
}

static const struct rt_i2c_bus_device_ops s31_i2c_ops =
{
    s31_i2c_xfer,
    RT_NULL,                 /* slave_xfer */
    s31_i2c_control,
};

/* 初始化一路 I2C：时钟 → 引脚 → 时序 → 主机模式 → 注册 */
static int s31_i2c_init_one(struct s31_i2c *i2c, const char *name, rt_uint32_t base,
                            rt_base_t scl_pad, rt_base_t sda_pad, rt_uint32_t sig_scl,
                            rt_uint32_t sig_sda, rt_uint32_t clkrst_reg, rt_uint32_t bus_hz)
{
    const rt_uint32_t src_hz = 40000000u;    /* CLK_SRC_SEL=0 → XTAL 40MHz */
    rt_uint32_t div, sclk, half, wait_high, high, ctrl;

    i2c->base = base;

    /* 1. 时钟：APB + 功能时钟都要开（复位默认 0），再给分频 */
    ctrl = S31_REG32(clkrst_reg);
    ctrl |= (CLKRST_APB_CLK_EN | CLKRST_CLK_EN);
    S31_REG32(clkrst_reg) = ctrl;
    ctrl |= CLKRST_RST_EN;                   /* 复位脉冲 */
    S31_REG32(clkrst_reg) = ctrl;
    ctrl &= ~CLKRST_RST_EN;
    ctrl &= ~CLKRST_CLK_SRC_SEL;             /* 源 = XTAL */
    div = src_hz / (bus_hz * 1024u) + 1u;    /* 照 IDF i2c_ll.h:104-128 的算法 */
    ctrl = (ctrl & ~(0xFFu << CLKRST_CLK_DIV_S)) | ((div - 1u) << CLKRST_CLK_DIV_S);
    S31_REG32(clkrst_reg) = ctrl;

    /* 2. 引脚 */
    i2c_set_pin(scl_pad, sig_scl);
    i2c_set_pin(sda_pad, sig_sda);

    /* 3. 时序（值 = 周期数 - 1；SCL_HIGH_PERIOD 里 high 与 wait_high 是两个字段） */
    sclk = src_hz / div;
    half = sclk / bus_hz / 2u;
    wait_high = (bus_hz >= 80000u) ? (half / 2u - 2u) : (half / 4u);
    high = half - wait_high;

    i2c_wr(i2c, I2C_SCL_LOW_PERIOD, half - 1u);
    i2c_wr(i2c, I2C_SCL_HIGH_PERIOD, ((wait_high & 0x7Fu) << 9) | (high & 0x1FFu));
    i2c_wr(i2c, I2C_SDA_HOLD, half / 4u - 1u);
    i2c_wr(i2c, I2C_SDA_SAMPLE, half / 2u - 1u);
    i2c_wr(i2c, I2C_SCL_START_HOLD, half - 1u);
    i2c_wr(i2c, I2C_SCL_RSTART_SETUP, half - 1u);
    i2c_wr(i2c, I2C_SCL_STOP_HOLD, half - 1u);
    i2c_wr(i2c, I2C_SCL_STOP_SETUP, half - 1u);
    i2c_wr(i2c, I2C_TO, (1u << 5) | 0x1Fu);  /* TIME_OUT_EN + 最大超时值 */

    /* 4. 主机模式、开漏（FORCE_OUT=0）、使能；最后 UPGATE 让配置生效
     * 中断使能打开（我们不用 ISR，但实测 NACK 等事件在 ENA=0 时不一定进 INT_RAW） */
    i2c_wr(i2c, I2C_CTR, I2C_CTR_MS_MODE | I2C_CTR_CLK_EN);
    i2c_wr(i2c, I2C_CTR, i2c_rd(i2c, I2C_CTR) | I2C_CTR_CONF_UPGATE);
    i2c_wr(i2c, I2C_INT_ENA, I2C_INT_TRANS_DONE | I2C_INT_NACK | I2C_INT_END_DETECT |
                             I2C_INT_TIME_OUT | I2C_INT_ARB_LOST);
    i2c_wr(i2c, I2C_INT_CLR, 0xFFFFu);
    i2c_fifo_reset(i2c);

    i2c->parent.ops  = &s31_i2c_ops;
    i2c->parent.priv = i2c;
    rt_i2c_bus_device_register(&i2c->parent, name);

    rt_kprintf("[i2c] %s ready: SCL=GPIO%d SDA=GPIO%d, %u Hz (div=%u half=%u)\n",
               name, (int)scl_pad, (int)sda_pad, (unsigned)bus_hz,
               (unsigned)div, (unsigned)half);
    return 0;
}

static int s31_i2c_init(void)
{
    /* 引脚：
     *   I2C0 → 板载 ES8311 那条总线：SCL=GPIO50、SDA=GPIO51（板上有真从机 0x18，
     *          正好用来验收；⚠️ 必须开内部上拉，见 i2c_set_pin 注释）
     *   I2C1 → 空闲脚 SCL=GPIO2(J2-7)、SDA=GPIO3(J2-10)，接外设用。
     *   🚨 原来用的是 45/46，但那是 SPI2 的 MISO/CS（drv_spi.c 默认接线）——
     *      两路驱动同时把同一个焊盘配成不同功能，谁后初始化谁赢（踩过）。*/
    s31_i2c_init_one(&s_i2c0, "i2c0", S31_I2C0_BASE, 50, 51,
                     I2C0_SCL_SIG, I2C0_SDA_SIG, CLKRST_I2C0_REG, 100000u);
    s31_i2c_init_one(&s_i2c1, "i2c1", S31_I2C1_BASE, 2, 3,
                     I2C1_SCL_SIG, I2C1_SDA_SIG, CLKRST_I2C1_REG, 100000u);
    return 0;
}
INIT_DEVICE_EXPORT(s31_i2c_init);

/* ---- 调试②：启动一次传输，同时用 CPU 采样焊盘电平 -------------------------
 * 这是"总线上到底有没有波形"的硬证据（不依赖 LA）：
 *   40MHz 的 CPU 对一个寄存器连续读，采样率 ~1MHz，抓 100kHz 的 I2C 绰绰有余。
 * 结果解释：
 *   changes>0 且 scl_low=1 → 总线在动，问题在时序/从机
 *   changes==0            → 硬件压根没发波形（命令没执行 / 引脚没接上 / 时钟没开）*/
void s31_i2c_wave(rt_uint8_t addr)
{
    struct s31_i2c *i2c = &s_i2c0;
    rt_uint32_t changes = 0, last, scl_low = 0, sda_low = 0, i;

    rt_kprintf("--- i2c0 wave test, addr 0x%02x ---\n", addr);

    /* 手动组一次"RSTART + WRITE(1) + STOP" */
    i2c_wr(i2c, I2C_INT_CLR, 0xFFFFu);
    i2c_fifo_reset(i2c);
    i2c_wr(i2c, I2C_DATA, ((rt_uint32_t)addr << 1));
    i2c_wr(i2c, I2C_COMD(0), i2c_cmd(I2C_OP_RSTART, 0, 0, 0, 0));
    i2c_wr(i2c, I2C_COMD(1), i2c_cmd(I2C_OP_WRITE, 1, 0, 0, 1));
    i2c_wr(i2c, I2C_COMD(2), i2c_cmd(I2C_OP_STOP, 0, 0, 0, 0));
    i2c_wr(i2c, I2C_CTR, i2c_rd(i2c, I2C_CTR) | I2C_CTR_CONF_UPGATE);

    last = (S31_REG32(GPIO_IN_REG_HIGH) >> 18) & 0x3u;    /* 采样前基线 */
    i2c_wr(i2c, I2C_CTR, i2c_rd(i2c, I2C_CTR) | I2C_CTR_TRANS_START);

    for (i = 0; i < 300000u; i++) {
        rt_uint32_t v = (S31_REG32(GPIO_IN_REG_HIGH) >> 18) & 0x3u;  /* bit0=SCL(50) bit1=SDA(51) */
        if (v != last) {
            changes++;
            last = v;
        }
        if (!(v & 1u)) { scl_low = 1; }
        if (!(v & 2u)) { sda_low = 1; }
    }

    rt_kprintf("samples=%u pin_changes=%u scl_ever_low=%u sda_ever_low=%u\n",
               (unsigned)i, (unsigned)changes, (unsigned)scl_low, (unsigned)sda_low);
    rt_kprintf("CTR=%08x (bit5 TRANS_START 此刻应为 0) SR=%08x INT_RAW=%08x\n",
               (unsigned)i2c_rd(i2c, I2C_CTR), (unsigned)i2c_rd(i2c, I2C_SR),
               (unsigned)i2c_rd(i2c, I2C_INT_RAW));
    rt_kprintf("COMD0=%08x COMD1=%08x COMD2=%08x  (bit31=DONE)\n",
               (unsigned)i2c_rd(i2c, I2C_COMD(0)), (unsigned)i2c_rd(i2c, I2C_COMD(1)),
               (unsigned)i2c_rd(i2c, I2C_COMD(2)));
    i2c_wr(i2c, I2C_INT_CLR, 0xFFFFu);
}
/* ---- 调试③：把一次探测的寄存器现场摊开（排查"全应答"/"全不应答"）
 * 用法： i2c_dbg <addr>   （默认 0x18） */
void s31_i2c_dbg(rt_uint8_t addr)
{
    struct s31_i2c *i2c = &s_i2c0;
    struct rt_i2c_msg m;
    rt_ssize_t r;

    rt_kprintf("--- i2c0 debug, addr 0x%02x ---\n", addr);
    rt_kprintf("pad idle: SCL(GPIO50)=%d SDA(GPIO51)=%d  (内部上拉应读到 1)\n",
               (int)((S31_REG32(GPIO_IN_REG_HIGH) >> (50 - 32)) & 1u),
               (int)((S31_REG32(GPIO_IN_REG_HIGH) >> (51 - 32)) & 1u));
    rt_kprintf("CTR=%08x SR=%08x INT_RAW=%08x INT_ENA=%08x\n",
               (unsigned)i2c_rd(i2c, I2C_CTR), (unsigned)i2c_rd(i2c, I2C_SR),
               (unsigned)i2c_rd(i2c, I2C_INT_RAW), (unsigned)i2c_rd(i2c, I2C_INT_ENA));
    rt_kprintf("SCL_LOW=%u SCL_HIGH=%08x SDA_HOLD=%u SDA_SAMPLE=%u TO=%08x\n",
               (unsigned)i2c_rd(i2c, I2C_SCL_LOW_PERIOD),
               (unsigned)i2c_rd(i2c, I2C_SCL_HIGH_PERIOD),
               (unsigned)i2c_rd(i2c, I2C_SDA_HOLD),
               (unsigned)i2c_rd(i2c, I2C_SDA_SAMPLE),
               (unsigned)i2c_rd(i2c, I2C_TO));
    rt_kprintf("clkrst=%08x (bit0 APB_EN, bit4 CLK_EN)\n",
               (unsigned)S31_REG32(CLKRST_I2C0_REG));

    m.addr  = addr;
    m.flags = RT_I2C_WR;
    m.len   = 0;
    m.buf   = RT_NULL;
    r = rt_i2c_transfer(&s_i2c0.parent, &m, 1);
    rt_kprintf("probe -> %d  (%s)\n", (int)r, (r == 1) ? "ACK/找到了" : "NACK/没设备");

    rt_kprintf("after: SR=%08x (bit0 RESP_REC 0=ACK/1=NACK, bit4 BUS_BUSY) INT_RAW=%08x\n",
               (unsigned)i2c_rd(i2c, I2C_SR), (unsigned)i2c_rd(i2c, I2C_INT_RAW));
    rt_kprintf("pad now : SCL=%d SDA=%d\n",
               (int)((S31_REG32(GPIO_IN_REG_HIGH) >> (50 - 32)) & 1u),
               (int)((S31_REG32(GPIO_IN_REG_HIGH) >> (51 - 32)) & 1u));
    rt_kprintf("COMD0=%08x COMD1=%08x COMD2=%08x\n",
               (unsigned)i2c_rd(i2c, I2C_COMD(0)), (unsigned)i2c_rd(i2c, I2C_COMD(1)),
               (unsigned)i2c_rd(i2c, I2C_COMD(2)));
}

/* ---- 调试④：连打 N 次同一个地址，把每次的信号与判定都摊开 ---------------
 * 用来抓"标志位竞争"：如果 NACK 位偶尔落在清理之后，会看到
 * 同一个地址几次结果不一样、且 SR.RESP_REC 与返回值可能矛盾。
 * 用法： i2c_rep <addr> [次数] */
void s31_i2c_rep(rt_uint8_t addr, int times)
{
    struct s31_i2c *i2c = &s_i2c0;
    int k;

    rt_kprintf("--- repeat probe 0x%02x x%d ---\n", addr, times);
    for (k = 0; k < times; k++) {
        struct rt_i2c_msg m;
        rt_ssize_t r;
        rt_uint32_t raw_before, sr_after, raw_after;

        raw_before = i2c_rd(i2c, I2C_INT_RAW);
        m.addr  = addr;
        m.flags = RT_I2C_WR;
        m.len   = 0;
        m.buf   = RT_NULL;
        r = rt_i2c_transfer(&s_i2c0.parent, &m, 1);
        sr_after  = i2c_rd(i2c, I2C_SR);
        raw_after = i2c_rd(i2c, I2C_INT_RAW);

        rt_kprintf("  #%d ret=%d | raw_before=%08x | SR=%08x RESP_REC=%u | raw_after=%08x\n",
                   k, (int)r, (unsigned)raw_before, (unsigned)sr_after,
                   (unsigned)(sr_after & 1u), (unsigned)raw_after);
    }
}
