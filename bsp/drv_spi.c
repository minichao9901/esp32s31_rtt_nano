/*===========================================================================
 * drv_spi.c -- GPSPI2 主机驱动（挂 RT-Thread 的 SPI 与 QSPI 框架）
 *
 * 接上框架后可用标准 API：
 *      rt_spi_configure(dev, &cfg);  rt_spi_transfer(dev, tx, rx, len);
 *      rt_spi_send_then_recv(...);   rt_qspi_send_then_recv(...);
 *
 * 寄存器（核对 IDF 6.1 的 soc/esp32s31/register/soc/spi_reg.h，
 * GPSPI2 基址 0x2038F000、GPSPI3 0x20390000）：
 *   CMD 0x00（bit24 USR 自清、bit23 UPDATE 自清）
 *   ADDR 0x04   CTRL 0x08   CLOCK 0x0C   USER 0x10   USER1 0x14   USER2 0x18
 *   MS_DLEN 0x1C（**MOSI/MISO 共用**，值 = 位数-1）
 *   MISC 0x20   DIN_MODE 0x24  DIN_NUM 0x28  DOUT_MODE 0x2C
 *   DMA_CONF 0x30（bit29 RX_AFIFO_RST、bit30 BUF_AFIFO_RST）
 *   DMA_INT_RAW 0x3C（bit12 TRANS_DONE）
 *   W0..W15 0x98..0xD4（16 字 = 64 字节 FIFO）
 *
 * 时钟：HP_SYS_CLKRST_GPSPI2_CTRL0 = 0x2058706C（bit0 SYS_CLK_EN、bit1 APB_CLK_EN
 *   复位默认都是 1；bit5:4 CLK_SRC_SEL：0=XTAL/1=RC_FAST/2=BBPLL；bit14:7 HS_DIV、
 *   bit22:15 MST_DIV、bit23 MST_CLK_EN）。默认就是 XTAL 40MHz ÷1 ÷1 = 40MHz，
 *   与 CPU 频率无关（升到 320MHz 也不变）。
 *   再串 SPI 自己的分频：sclk = 40MHz / (CLKDIV_PRE+1) / (CLKCNT_N+1)
 *
 * 引脚：默认走 **GPIO matrix**，用的就是本工作区 spi_flash_sfud 那套接线
 *   （SCK=GPIO43 MOSI=44 MISO=45 CS=46，都在 J2 上），这样手上那块 W25Q64 模块
 *   直接能用。也支持 GPSPI2 的专用 IO_MUX 脚（GPIO20~25，MCU_SEL=2）。
 *
 * ⚠️ PIO 模式单次最多 64 字节（FIFO 16 字）。要更长得用 DMA（AXI GDMA，trigger=1）。
 * ⚠️ 四线(QIO)要用 IO2/IO3 = WP/HD 两根额外的线，**4 线接线做不了四线模式**
 *   （那两根没接时数据相位只有 2 根线在工作，读回来是垃圾）。
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "s31_regs.h"

#define S31_SPI2_BASE       0x2038F000u
#define S31_SPI3_BASE       0x20390000u
#define CLKRST_GPSPI2_REG   (S31_HP_SYS_CLKRST_BASE + 0x6Cu)
#define CLKRST_GPSPI3_REG   (S31_HP_SYS_CLKRST_BASE + 0x70u)

/* 寄存器偏移 */
#define SPI_CMD        0x00u
#define SPI_ADDR       0x04u
#define SPI_CTRL       0x08u
#define SPI_CLOCK      0x0Cu
#define SPI_USER       0x10u
#define SPI_USER1      0x14u
#define SPI_USER2      0x18u
#define SPI_MS_DLEN    0x1Cu
#define SPI_MISC       0x20u
#define SPI_DMA_CONF   0x30u
#define SPI_DMA_INT_RAW 0x3Cu
#define SPI_W(n)       (0x98u + 4u * (n))

/* CMD */
#define SPI_CMD_UPDATE (1u << 23)
#define SPI_CMD_USR    (1u << 24)
/* USER */
#define SPI_USER_DOUTDIN      (1u << 0)
#define SPI_USER_QPI_MODE     (1u << 3)
#define SPI_USER_CS_HOLD      (1u << 6)
#define SPI_USER_CS_SETUP     (1u << 7)
#define SPI_USER_CK_OUT_EDGE  (1u << 9)
#define SPI_USER_FWRITE_DUAL  (1u << 12)
#define SPI_USER_FWRITE_QUAD  (1u << 13)
#define SPI_USER_USR_MOSI     (1u << 27)
#define SPI_USER_USR_MISO     (1u << 28)
#define SPI_USER_USR_DUMMY    (1u << 29)
#define SPI_USER_USR_ADDR     (1u << 30)
#define SPI_USER_USR_COMMAND  (1u << 31)
/* USER1 */
#define SPI_USER1_DUMMY_CYCLELEN_S 0
#define SPI_USER1_ADDR_BITLEN_S    27
/* USER2 */
#define SPI_USER2_COMMAND_VALUE_S  0
#define SPI_USER2_COMMAND_BITLEN_S 28
/* CTRL */
#define SPI_CTRL_FADDR_DUAL   (1u << 5)
#define SPI_CTRL_FADDR_QUAD   (1u << 6)
#define SPI_CTRL_FCMD_DUAL    (1u << 8)
#define SPI_CTRL_FCMD_QUAD    (1u << 9)
#define SPI_CTRL_FREAD_DUAL   (1u << 14)
#define SPI_CTRL_FREAD_QUAD   (1u << 15)
#define SPI_CTRL_WP_POL       (1u << 21)
#define SPI_CTRL_HOLD_POL     (1u << 20)
/* CLOCK */
#define SPI_CLOCK_EQU_SYSCLK  (1u << 31)
/* MISC */
#define SPI_MISC_CS_KEEP_ACTIVE (1u << 30)
#define SPI_MISC_CK_IDLE_EDGE   (1u << 29)
/* DMA_CONF */
#define SPI_DMA_RX_AFIFO_RST  (1u << 29)
#define SPI_DMA_BUF_AFIFO_RST (1u << 30)
/* DMA_INT */
#define SPI_TRANS_DONE        (1u << 12)

/* GPIO matrix（复用 drv_gpio.c 那套） */
#define S31_GPIO_BASE        0x20583000u
#define GPIO_FUNC_OUT_SEL(n) (S31_GPIO_BASE + 0xAF4u + 4u * (n))
#define GPIO_FUNC_IN_SEL(m)  (S31_GPIO_BASE + 0x2F4u + 4u * (m))
#define GPIO_IN_SEL_MATRIX   (1u << 9)
#define SIG_GPIO_OUT_REG     256u    /* OUT_SEL 的一个特殊值：输出 = GPIO_OUT 寄存器 */
#define SPI2_CK_SIG          53u
#define SPI2_D_SIG           55u      /* MOSI */
#define SPI2_Q_SIG           54u      /* MISO */
#define SPI2_CS0_SIG         62u
#define SPI2_HOLD_SIG        56u
#define SPI2_WP_SIG          57u

/* 本驱动用的焊盘（都在 J2 上，和本工作区 spi_flash_sfud 那套接线一致）*/
#define S31_SPI2_SCK_PAD     43
#define S31_SPI2_MOSI_PAD    44
#define S31_SPI2_MISO_PAD    45
#define S31_SPI2_CS_PAD      46
/* 外接 SPI 屏（AXS15352）那档：CS 用 GPIO45（见 spi_lcd_* 工程的接线）*/
#define LCD_CS_PAD           45

extern rt_uint64_t s31_systimer_get_ticks(void);

/* 焊盘小工具（定义在文件后半段，环回自检要用）*/
static void spi_set_pad_ie(rt_base_t pad, int enable);
void s31_spi_miso_pull(int enable);

/* IO_MUX */
#include "s31_iomux_table.h"
#define S31_IO_MUX_BASE      0x20582000u
#define IOMUX_MCU_SEL_S      12
#define IOMUX_FUN_IE         (1u << 9)
#define IOMUX_FUN_PU         (1u << 8)
#define IOMUX_FUN_PD         (1u << 7)
#define IOMUX_PIN_FUNC_GPIO  1u

/* 硬件状态（只有一份：spi2 与 qspi2 两条 RT-Thread 总线共用同一组寄存器）*/
struct s31_spi_hw
{
    rt_uint32_t base;
    rt_uint32_t src_hz;
};

static struct s31_spi_hw s_spi2_hw;

/* 两条总线对象：SPI 消息走 s31_spi_xfer、QSPI 消息走 s31_qspi_xfer。
 * （同一个 struct rt_spi_bus 不能注册两次——设备对象表里会重名。）*/
static struct rt_spi_bus  s_spi2_bus;
static struct rt_spi_bus  s_qspi2_bus;

static inline rt_uint32_t spi_rd(struct s31_spi_hw *spi, rt_uint32_t off)
{
    return S31_REG32(spi->base + off);
}

static inline void spi_wr(struct s31_spi_hw *spi, rt_uint32_t off, rt_uint32_t v)
{
    S31_REG32(spi->base + off) = v;
}

/* ---- 引脚 --------------------------------------------------------------- */

/* 焊盘 → GPIO matrix 输出（MCU_SEL=1 走 GPIO，OUT_SEL 指到外设信号）*/
static void spi_mux_out(rt_base_t pad, rt_uint32_t sig)
{
    rt_uint32_t off, v;

    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    off = s31_iomux_off[pad];
    if (off == 0xFFFFFFFFu) {
        return;
    }
    v  = S31_REG32(S31_IO_MUX_BASE + off);
    v &= ~((0x7u << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE | IOMUX_FUN_PU | IOMUX_FUN_PD);
    v |= (IOMUX_PIN_FUNC_GPIO << IOMUX_MCU_SEL_S);
    S31_REG32(S31_IO_MUX_BASE + off) = v;

    /* oen_sel(bit10)=0 → 输出使能由 GPIO_ENABLE 管（SPI 输出脚常开）*/
    S31_REG32(GPIO_FUNC_OUT_SEL(pad)) = sig & 0x1FFu;
}

/* 外设输入信号 ← 焊盘（GPIO matrix 输入，bit9=1 表示来自 matrix）*/
static void spi_mux_in(rt_base_t pad, rt_uint32_t sig)
{
    rt_uint32_t off, v;

    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    off = s31_iomux_off[pad];
    if (off == 0xFFFFFFFFu) {
        return;
    }
    v  = S31_REG32(S31_IO_MUX_BASE + off);
    v &= ~((0x7u << IOMUX_MCU_SEL_S) | IOMUX_FUN_PU | IOMUX_FUN_PD);
    v |= (IOMUX_PIN_FUNC_GPIO << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE;
    S31_REG32(S31_IO_MUX_BASE + off) = v;

    S31_REG32(GPIO_FUNC_IN_SEL(sig)) = ((rt_uint32_t)pad & 0x3Fu) | GPIO_IN_SEL_MATRIX;
}

/* 用 GPIO matrix 接线（默认就是 sfud 工程那套：43/44/45/46）
 * 🚨 CS 也走**外设信号**（SPI2_CS0 = 62），不是普通 GPIO：
 *   实测（2026-09-23）用 `s31_reg` 回读发现开机后 CS 焊盘的 OUT_SEL 是 0x47、
 *   且 GPIO_ENABLE1 的对应位是 0 —— 也就是说"手动 GPIO 拉 CS"那条路当时**根本没生效**
 *   （CS 一直浮空），SPI 事务其实是在"片选永不有效"的状态下跑的。
 *   交给硬件 CS0 还有额外好处：CS 的 setup/hold 由外设按 CS_SETUP/CS_HOLD 控制，
 *   比软件写 GPIO 更贴近真实时序。跨消息保持低电平靠 SPI_MISC 的 CS_KEEP_ACTIVE。*/
static void spi_set_pins_matrix(rt_base_t sck, rt_base_t mosi, rt_base_t miso, rt_base_t cs)
{
    spi_mux_out(sck,  SPI2_CK_SIG);          /* CLK 输出 */
    spi_mux_out(mosi, SPI2_D_SIG);           /* MOSI 输出 */
    spi_mux_in(miso,  SPI2_Q_SIG);           /* MISO 输入 */
    spi_mux_out(cs,   SPI2_CS0_SIG);         /* CS0（外设硬件片选，见上）*/
}

/* 把一个焊盘还原成"普通 GPIO、不驱动"（切换接线方式时收拾现场用）*/
static void spi_pin_release(rt_base_t pad)
{
    rt_uint32_t off, v;

    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    off = s31_iomux_off[pad];
    if (off == 0xFFFFFFFFu) {
        return;
    }
    v  = S31_REG32(S31_IO_MUX_BASE + off);
    v &= ~((0x7u << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE | IOMUX_FUN_PU | IOMUX_FUN_PD);
    v |= (IOMUX_PIN_FUNC_GPIO << IOMUX_MCU_SEL_S);
    S31_REG32(S31_IO_MUX_BASE + off) = v;
    S31_REG32(GPIO_FUNC_OUT_SEL(pad)) = SIG_GPIO_OUT_REG;    /* 输出源 = GPIO_OUT */
    S31_REG32(GPIO_FUNC_IN_SEL(pad)) = 0;                    /* 输入信号与它无关 */
}

/* ---- 专用 IO_MUX 接线（真四线 QIO 必须走这组脚）------------------------- */
/* 出处：IDF `esp_hal_gpspi/esp32s31/include/soc/spi_pins.h`
 *   SPI2_FUNC_NUM_QUAD = 2：CLK=20 MOSI=21 MISO=22 CS0=23 HD=24 WP=25
 *   （第二组是 52~57，那是板上 I2S/音频脚，不能用）
 * 为什么四线必须用这组：IDF 的 `check_iomux_pins_quad()` 明确要求四根数据线/时钟
 *   必须是外设的 IO_MUX 脚 —— GPIO matrix 下 4 根线在数据相位**没法三态**
 *   （oen_sel=0 时输出使能由 GPIO_ENABLE 管，从机驱不动线）。
 * 这 6 个脚在板上是 SDIO 的 SD_D0~D3/CLK/CMD（J2 的 26~30 脚，板上没卡座）。*/
#define S31_SPI2_IOMUX_CLK   20
#define S31_SPI2_IOMUX_MOSI  21
#define S31_SPI2_IOMUX_MISO  22
#define S31_SPI2_IOMUX_CS    23
#define S31_SPI2_IOMUX_HD    24
#define S31_SPI2_IOMUX_WP    25
#define S31_SPI2_IOMUX_FUNC  2      /* MCU_SEL = SPI2_FUNC_NUM_QUAD */

static const rt_base_t s_spi2_iomux_pads[6] = {
    S31_SPI2_IOMUX_CLK, S31_SPI2_IOMUX_MOSI, S31_SPI2_IOMUX_MISO,
    S31_SPI2_IOMUX_CS,  S31_SPI2_IOMUX_HD,   S31_SPI2_IOMUX_WP
};

/* 0 = GPIO matrix（手动 CS），1 = 专用 IO_MUX（硬件 CS0）*/
static int s_spi_iomux_mode = 0;

static void spi_set_pins_iomux(void)
{
    rt_uint32_t i;

    for (i = 0; i < 6; i++) {
        rt_base_t pad = s_spi2_iomux_pads[i];
        rt_uint32_t off = s31_iomux_off[pad];
        rt_uint32_t v;

        if (off == 0xFFFFFFFFu) {
            continue;
        }
        v  = S31_REG32(S31_IO_MUX_BASE + off);
        v &= ~((0x7u << IOMUX_MCU_SEL_S) | IOMUX_FUN_PU | IOMUX_FUN_PD);
        v |= ((rt_uint32_t)S31_SPI2_IOMUX_FUNC << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE;
        S31_REG32(S31_IO_MUX_BASE + off) = v;
    }
    s_spi_iomux_mode = 1;
}

/* ---- CS：两种接线都用硬件 CS0 + CS_KEEP_ACTIVE -------------------------- */
/* CS 焊盘接的都是 GPSPI2 的 CS0 信号（matrix 模式走信号 62 到 GPIO46；
 * IO_MUX 模式直接是 GPIO23 的专用功能）。硬件在每次 USR 期间自动拉低 CS，
 * 一条消息里的多次传输之间靠 CS_KEEP_ACTIVE 保持低（不然 send_then_recv
 * 中间会抬起来，flash 的命令+数据就断成两帧了）。*/
static void spi_cs_assert(struct s31_spi_hw *spi)
{
    spi_wr(spi, SPI_MISC, spi_rd(spi, SPI_MISC) | SPI_MISC_CS_KEEP_ACTIVE);
}

static void spi_cs_release(struct s31_spi_hw *spi)
{
    spi_wr(spi, SPI_MISC, spi_rd(spi, SPI_MISC) & ~SPI_MISC_CS_KEEP_ACTIVE);
}

/* ---- 时钟分频 ----------------------------------------------------------- */

/* 照 IDF spi_ll_master_cal_clock 的思路找最接近的 (pre, n)。
 * ⚠️ S31 的 CLKDIV_PRE 只有 **4 位（1~16）**，不是老 ESP32 的 6 位（1~64）——
 *    实测教训：按 6 位算写 pre=20，硬件截成 pre=4，1MHz 请求实际跑出 5MHz
 *    （环回自检用 SYSTIMER 量位率才发现：1MHz 和 5MHz 两档耗时一模一样）。*/
#define S31_SPI_PRE_MAX  16u

static rt_uint32_t spi_calc_clock(rt_uint32_t src_hz, rt_uint32_t hz,
                                  rt_uint32_t *pre_out, rt_uint32_t *n_out)
{
    rt_uint32_t n, pre, f, err, best_err = 0xFFFFFFFFu, best_pre = 1, best_n = 2;

    if (hz == 0 || hz >= src_hz) {
        *pre_out = 1;
        *n_out = 2;
        return src_hz / 2u;
    }
    for (n = 2; n <= 64; n++) {
        pre = (src_hz / n + hz / 2u) / hz;
        if (pre < 1) {
            pre = 1;
        }
        if (pre > S31_SPI_PRE_MAX) {
            pre = S31_SPI_PRE_MAX;
        }
        f = src_hz / (pre * n);
        err = (f > hz) ? (f - hz) : (hz - f);
        if (err < best_err) {
            best_err = err;
            best_pre = pre;
            best_n = n;
        }
    }
    *pre_out = best_pre;
    *n_out = best_n;
    return src_hz / (best_pre * best_n);
}

static rt_err_t s31_spi_configure(struct rt_spi_device *device, struct rt_spi_configuration *cfg)
{
    struct s31_spi_hw *spi = &s_spi2_hw;
    rt_uint32_t pre, n, h, user, eff, width = cfg->data_width;

    /* 🚨 data_width == 0 要当成 8 收下：RT-Thread 的 QSPI 路径会把
     * `struct rt_spi_device.config`（**不是** rt_qspi_device.config.parent）
     * 传给驱动的 configure（dev_spi_core.c:179 / dev_qspi_core.c:102），
     * 而 rt_qspi_configure() 只写后者 —— 前者没被赋过值就是全 0。
     * 早期版本这里直接返回 -RT_EIO，于是 QSPI 的时钟/线宽配置**静默不生效**
     * （现象：qspi 帧的快慢跟着上一次 SPI 的配置走，踩过）。*/
    if (width == 0) {
        width = 8;
    }
    if (width != 8) {
        return -RT_EIO;                      /* 只支持 8 位 */
    }

    /* 高频档（> 3/4 源频）走 CLK_EQU_SYSCLK：直接输出源时钟 */
    if (cfg->max_hz > (spi->src_hz / 4u) * 3u) {
        pre = 1;
        n   = 1;
        eff = spi->src_hz;
        spi_wr(spi, SPI_CLOCK, SPI_CLOCK_EQU_SYSCLK);
    } else {
        eff = spi_calc_clock(spi->src_hz, cfg->max_hz, &pre, &n);
        h = (n >= 2) ? (n / 2u - 1u) : 0u;
        spi_wr(spi, SPI_CLOCK,
               ((pre - 1u) & 0xFu) << 18 |
               ((n   - 1u) & 0x3Fu) << 12 |
               (h & 0x3Fu) << 6 |
               ((n - 1u) & 0x3Fu));
    }

    /* 模式：mode0 ck_idle=0/out_edge=0；mode1 0/1；mode2 1/1；mode3 1/0 */
    user = SPI_USER_DOUTDIN | SPI_USER_USR_MOSI | SPI_USER_USR_MISO |
           SPI_USER_CS_HOLD | SPI_USER_CS_SETUP;
    if (cfg->mode & RT_SPI_CPOL) {
        spi_wr(spi, SPI_MISC, spi_rd(spi, SPI_MISC) | SPI_MISC_CK_IDLE_EDGE);
    } else {
        spi_wr(spi, SPI_MISC, spi_rd(spi, SPI_MISC) & ~SPI_MISC_CK_IDLE_EDGE);
    }
    if ((cfg->mode & RT_SPI_CPHA) ^ (cfg->mode & RT_SPI_CPOL)) {
        user |= SPI_USER_CK_OUT_EDGE;
    }
    spi_wr(spi, SPI_USER, user);
    spi_wr(spi, SPI_CTRL,
           SPI_CTRL_HOLD_POL | SPI_CTRL_WP_POL);   /* 空闲电平；这两脚没接也不影响单线 */
    spi_wr(spi, SPI_MISC, (spi_rd(spi, SPI_MISC) & SPI_MISC_CK_IDLE_EDGE) | 0x3Eu);
                                                /* CS0 使能、CS1~5 关（我们用手动 CS）*/

    rt_kprintf("[spi] %s: %u Hz -> pre=%u n=%u (实际 %u Hz) mode=%u\n",
               device->parent.parent.name, (unsigned)cfg->max_hz,
               (unsigned)pre, (unsigned)n, (unsigned)eff, (unsigned)cfg->mode);
    return RT_EOK;
}

/* ---- 底层：一次 full-duplex（≤64 字节）--------------------------------- */

static void spi_start(struct s31_spi_hw *spi)
{
    spi_wr(spi, SPI_DMA_INT_RAW, 0xFFFFFFFFu);          /* 清 TRANS_DONE */
    spi_wr(spi, SPI_CMD, SPI_CMD_UPDATE);
    while (spi_rd(spi, SPI_CMD) & SPI_CMD_UPDATE) {
    }
    spi_wr(spi, SPI_CMD, SPI_CMD_USR);
    while (spi_rd(spi, SPI_CMD) & SPI_CMD_USR) {
    }
    spi_wr(spi, SPI_DMA_INT_RAW, 0xFFFFFFFFu);
}

static void spi_hw_transfer(struct s31_spi_hw *spi, const rt_uint8_t *tx,
                            rt_uint8_t *rx, rt_uint32_t len)
{
    rt_uint32_t i, k, w;

    /* FIFO 复位（长度寄存器每次都要重设） */
    spi_wr(spi, SPI_DMA_CONF, SPI_DMA_RX_AFIFO_RST | SPI_DMA_BUF_AFIFO_RST);
    spi_wr(spi, SPI_DMA_CONF, 0);

    spi_wr(spi, SPI_MS_DLEN, len * 8u - 1u);

    for (i = 0; i < len; i += 4) {
        w = 0;
        for (k = 0; k < 4 && (i + k) < len; k++) {
            ((rt_uint8_t *)&w)[k] = tx ? tx[i + k] : 0xFFu;   /* 读时 MOSI 给 0xFF */
        }
        spi_wr(spi, SPI_W(i / 4), w);
    }

    spi_start(spi);

    if (rx) {
        for (i = 0; i < len; i += 4) {
            w = spi_rd(spi, SPI_W(i / 4));
            for (k = 0; k < 4 && (i + k) < len; k++) {
                rx[i + k] = ((rt_uint8_t *)&w)[k];
            }
        }
    }
}

/* ---- QSPI：命令/地址/空转/数据各阶段线宽 --------------------------------- */

static void qspi_hw_transfer(struct s31_spi_hw *spi, struct rt_qspi_message *q)
{
    rt_uint32_t user = 0, ctrl = SPI_CTRL_HOLD_POL | SPI_CTRL_WP_POL;
    rt_uint32_t user1 = 0, user2 = 0;
    rt_uint32_t len = q->parent.length;

    spi_wr(spi, SPI_DMA_CONF, SPI_DMA_RX_AFIFO_RST | SPI_DMA_BUF_AFIFO_RST);
    spi_wr(spi, SPI_DMA_CONF, 0);
    spi_wr(spi, SPI_ADDR, 0);

    if (q->instruction.content) {
        user |= SPI_USER_USR_COMMAND;
        user2 |= ((rt_uint32_t)q->instruction.content & 0xFFFFu);
        user2 |= (7u << SPI_USER2_COMMAND_BITLEN_S);            /* 8 位命令 */
        if (q->instruction.qspi_lines == 4) {
            ctrl |= SPI_CTRL_FCMD_QUAD;
        } else if (q->instruction.qspi_lines == 2) {
            ctrl |= SPI_CTRL_FCMD_DUAL;
        }
    }
    if (q->address.size) {
        user |= SPI_USER_USR_ADDR;
        spi_wr(spi, SPI_ADDR, q->address.content << (32u - q->address.size));
        user1 |= ((q->address.size - 1u) & 0x1Fu) << SPI_USER1_ADDR_BITLEN_S;
        if (q->address.qspi_lines == 4) {
            ctrl |= SPI_CTRL_FADDR_QUAD;
        } else if (q->address.qspi_lines == 2) {
            ctrl |= SPI_CTRL_FADDR_DUAL;
        }
    }
    if (q->dummy_cycles) {
        user |= SPI_USER_USR_DUMMY;
        user1 |= ((q->dummy_cycles - 1u) & 0xFFu) << SPI_USER1_DUMMY_CYCLELEN_S;
    }

    if (q->parent.recv_buf) {
        user |= SPI_USER_USR_MISO;
        if (q->qspi_data_lines == 4) {
            ctrl |= SPI_CTRL_FREAD_QUAD;
        } else if (q->qspi_data_lines == 2) {
            ctrl |= SPI_CTRL_FREAD_DUAL;
        }
    } else {
        user |= SPI_USER_USR_MOSI;
        if (q->qspi_data_lines == 4) {
            user |= SPI_USER_FWRITE_QUAD;
        } else if (q->qspi_data_lines == 2) {
            user |= SPI_USER_FWRITE_DUAL;
        }
    }

    spi_wr(spi, SPI_USER,  user);
    spi_wr(spi, SPI_USER1, user1);
    spi_wr(spi, SPI_USER2, user2);
    spi_wr(spi, SPI_CTRL,  ctrl);
    spi_wr(spi, SPI_MS_DLEN, len * 8u - 1u);

    if (q->parent.send_buf) {
        rt_uint32_t i, k, w;
        for (i = 0; i < len; i += 4) {
            w = 0;
            for (k = 0; k < 4 && (i + k) < len; k++) {
                ((rt_uint8_t *)&w)[k] = ((const rt_uint8_t *)q->parent.send_buf)[i + k];
            }
            spi_wr(spi, SPI_W(i / 4), w);
        }
    }

    spi_start(spi);

    if (q->parent.recv_buf) {
        rt_uint32_t i, k, w;
        for (i = 0; i < len; i += 4) {
            w = spi_rd(spi, SPI_W(i / 4));
            for (k = 0; k < 4 && (i + k) < len; k++) {
                ((rt_uint8_t *)q->parent.recv_buf)[i + k] = ((rt_uint8_t *)&w)[k];
            }
        }
    }
}

/* ---- 自环回自检（matrix 模式不需要任何外部器件）------------------------- */

/* matrix 模式：把 SPI2 的 Q（MISO）输入信号临时改接到 MOSI 那个焊盘上；
 * IO_MUX 模式：做不了内部环回（Q 固定接到 MISO 脚），需要一根 MOSI→MISO 跳线。
 * 两种情况下"发出去的数据原样收回来"都一次验完「时钟分频 → 移位 → D 输出 →
 * Q 输入采样 → MS_DLEN 长度 → FIFO 收发」整条通路（真实从机不在也能自证），
 * 并用 SYSTIMER（16MHz，与 CPU 频率无关）量实际位率，验证分频算得对。*/
int s31_spi_loopback(rt_uint32_t hz)
{
    struct rt_spi_device *dev = (struct rt_spi_device *)rt_device_find("flash0");
    struct rt_spi_configuration cfg;
    static rt_uint8_t tx[64], rx[64];
    rt_uint32_t i, bad = 0, first = 0xFFFFFFFFu, us, bitrate, bitns;
    rt_uint64_t t0, t1;

    if (dev == RT_NULL) {
        rt_kprintf("[spi] 找不到设备 flash0（drv_spi 没初始化？）\n");
        return -RT_ERROR;
    }

    cfg.data_width = 8;
    cfg.mode       = RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz     = hz;
    rt_spi_configure(dev, &cfg);

    if (s_spi_iomux_mode) {
        rt_kprintf("[spi] 环回（IO_MUX 模式）：需要一根跳线把 MOSI(GPIO%d) 接到 MISO(GPIO%d)\n",
                   S31_SPI2_IOMUX_MOSI, S31_SPI2_IOMUX_MISO);
    } else {
        /* 环回：MISO 的输入源 = MOSI 焊盘（该焊盘要额外打开输入缓冲）*/
        spi_set_pad_ie(S31_SPI2_MOSI_PAD, 1);
        S31_REG32(GPIO_FUNC_IN_SEL(SPI2_Q_SIG)) =
            ((rt_uint32_t)S31_SPI2_MOSI_PAD & 0x3Fu) | GPIO_IN_SEL_MATRIX;
    }

    for (i = 0; i < sizeof(tx); i++) {
        tx[i] = (rt_uint8_t)(0xA5u ^ (i * 7u) ^ (i << 3));
    }
    rt_memset(rx, 0, sizeof(rx));

    t0 = s31_systimer_get_ticks();
    rt_spi_transfer(dev, tx, rx, sizeof(tx));
    t1 = s31_systimer_get_ticks();

    for (i = 0; i < sizeof(tx); i++) {
        if (tx[i] != rx[i]) {
            if (first == 0xFFFFFFFFu) {
                first = i;
            }
            bad++;
        }
    }

    if (!s_spi_iomux_mode) {
        /* 恢复：MISO 仍从 GPIO45 取 */
        S31_REG32(GPIO_FUNC_IN_SEL(SPI2_Q_SIG)) =
            ((rt_uint32_t)S31_SPI2_MISO_PAD & 0x3Fu) | GPIO_IN_SEL_MATRIX;
        spi_set_pad_ie(S31_SPI2_MOSI_PAD, 0);
    }

    us = (rt_uint32_t)((t1 - t0) / (S31_SYSTIMER_HZ / 1000000u));
    if (us == 0) {
        us = 1;
    }
    bitrate = (rt_uint32_t)(sizeof(tx) * 8u) * 1000000u / us;
    bitns   = us * 1000u / (rt_uint32_t)(sizeof(tx) * 8u);

    rt_kprintf("[spi] 环回 %u Hz: 64B 全双工 %u us → %u bit/s、%u ns/bit（含软件开销，恒 ≤ 设定值）\n",
               (unsigned)hz, (unsigned)us, (unsigned)bitrate, (unsigned)bitns);

    if (bad == 0) {
        rt_kprintf("[spi] 环回数据 64/64 字节一致 PASS（前 4 字节 %02X %02X %02X %02X）\n",
                   rx[0], rx[1], rx[2], rx[3]);
        return RT_EOK;
    }
    rt_kprintf("[spi] 环回数据 %u/64 字节不符 FAIL（首个 i=%u tx=%02X rx=%02X）\n",
               (unsigned)bad, (unsigned)first,
               (first < sizeof(tx)) ? tx[first] : 0, (first < sizeof(tx)) ? rx[first] : 0);
    return -RT_ERROR;
}

/* QSPI 配置同步（**必须调**，否则 QSPI 的时钟/线宽设置等于没设）：
 * RT-Thread 把 QSPI 设备的配置分成了两份 ——
 *   `rt_qspi_device.config.parent`（rt_qspi_configure 写的）
 *   `rt_spi_device.parent.config`（驱动 configure() 实际读到的那份）
 * 而 `rt_qspi_configure()` 只写前者，`rt_spi_bus_configure()` 却把后者传给驱动
 * （dev_qspi_core.c:69 → dev_spi_core.c:179）。两份不同步时，驱动一直用旧配置，
 * 现象是"QSPI 一帧的快慢跟着上一次普通 SPI 的配置走"（踩过）。 */
void s31_qspi_sync_config(struct rt_qspi_device *dev)
{
    dev->parent.config = dev->config.parent;
    rt_spi_bus_configure(&dev->parent);
}

/* ---- 接线方式切换 + 寄存器回读（msh 用）--------------------------------- */
/* mode: 0 = GPIO matrix（SCK43/MOSI44/MISO45/CS46，手动 CS）
 *       1 = 专用 IO_MUX（CLK20/MOSI21/MISO22/CS23/HD24/WP25，硬件 CS0）
 *       2 = **LCD 档**：SCK=43 / MOSI=44 / CS=45（硬件 CS0），不占 MISO
 *           —— 外接 SPI 屏（AXS15352）就是这套线：45 当片选、46/47/48 做 DC/RST/BL
 * 返回 0 成功。⚠️ 切换会先把**另一组**焊盘还原成普通 GPIO，避免两组同时驱动。*/
int s31_spi_set_pins(int mode)
{
    if (mode == 2) {
        /* 收拾 matrix 组里不用的两根（45 要改当 CS，46 让给 DC）*/
        spi_pin_release(S31_SPI2_MISO_PAD);
        spi_pin_release(S31_SPI2_CS_PAD);
        s_spi_iomux_mode = 0;
        spi_mux_out(S31_SPI2_SCK_PAD,  SPI2_CK_SIG);      /* 43 = CLK */
        spi_mux_out(S31_SPI2_MOSI_PAD, SPI2_D_SIG);       /* 44 = MOSI */
        spi_mux_out(LCD_CS_PAD,        SPI2_CS0_SIG);     /* 45 = CS0（硬件片选）*/
        return 0;
    }

    if (mode) {
        /* 收拾 matrix 那组 */
        spi_pin_release(S31_SPI2_SCK_PAD);
        spi_pin_release(S31_SPI2_MOSI_PAD);
        spi_pin_release(S31_SPI2_MISO_PAD);
        spi_pin_release(S31_SPI2_CS_PAD);
        spi_set_pins_iomux();
    } else {
        rt_uint32_t i;
        for (i = 0; i < 6; i++) {
            spi_pin_release(s_spi2_iomux_pads[i]);
        }
        s_spi_iomux_mode = 0;
        spi_set_pins_matrix(S31_SPI2_SCK_PAD, S31_SPI2_MOSI_PAD,
                            S31_SPI2_MISO_PAD, S31_SPI2_CS_PAD);
    }

    /* 接线换了，总线上的设备配置作废：下次传输会按 device->config 重新配 */
    return 0;
}

/* 把当前 6 个焊盘的 IO_MUX / 输出选择寄存器打出来（接线对不对看这个）*/
void s31_spi_dump_pins(void)
{
    static const struct { const char *name; rt_base_t pad; } list[6] = {
        {"CLK ", S31_SPI2_IOMUX_CLK}, {"MOSI", S31_SPI2_IOMUX_MOSI},
        {"MISO", S31_SPI2_IOMUX_MISO}, {"CS  ", S31_SPI2_IOMUX_CS},
        {"HD  ", S31_SPI2_IOMUX_HD},  {"WP  ", S31_SPI2_IOMUX_WP},
    };
    rt_uint32_t i;

    rt_kprintf("[spi] 当前接线 = %s\n", s_spi_iomux_mode ? "专用 IO_MUX(20~25)" : "GPIO matrix(43/44/45/46)");
    rt_kprintf("[spi] -- IO_MUX 组（MCU_SEL 应为 %u 才是外设专用功能）--\n", S31_SPI2_IOMUX_FUNC);
    for (i = 0; i < 6; i++) {
        rt_base_t pad = list[i].pad;
        rt_uint32_t off = s31_iomux_off[pad];
        rt_uint32_t v = (off == 0xFFFFFFFFu) ? 0u : S31_REG32(S31_IO_MUX_BASE + off);
        rt_kprintf("      %s GPIO%-3d iomux=%08x MCU_SEL=%u IE=%u  OUT_SEL=%08x\n",
                   list[i].name, (int)pad, (unsigned)v,
                   (unsigned)((v >> IOMUX_MCU_SEL_S) & 0x7u),
                   (unsigned)((v & IOMUX_FUN_IE) ? 1u : 0u),
                   (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(pad)));
    }
    rt_kprintf("[spi] -- matrix 组（SCK43/MOSI44/MISO45/CS46）--\n");
    for (i = 0; i < 4; i++) {
        rt_base_t pad = (i == 0) ? S31_SPI2_SCK_PAD : (i == 1) ? S31_SPI2_MOSI_PAD :
                        (i == 2) ? S31_SPI2_MISO_PAD : S31_SPI2_CS_PAD;
        rt_uint32_t off = s31_iomux_off[pad];
        rt_uint32_t v = (off == 0xFFFFFFFFu) ? 0u : S31_REG32(S31_IO_MUX_BASE + off);
        rt_kprintf("      GPIO%-3d iomux=%08x MCU_SEL=%u IE=%u  OUT_SEL=%08x  IN54=%08x\n",
                   (int)pad, (unsigned)v,
                   (unsigned)((v >> IOMUX_MCU_SEL_S) & 0x7u),
                   (unsigned)((v & IOMUX_FUN_IE) ? 1u : 0u),
                   (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(pad)),
                   (unsigned)S31_REG32(GPIO_FUNC_IN_SEL(SPI2_Q_SIG)));
    }
}

/* MISO 焊盘内部下拉开关：用来做"线上到底有没有器件"的判别实验 ——
 * 浮空脚在 45k 下拉下必然读 0；真有从机在驱动 Q 时下拉赢不了。*/void s31_spi_miso_pull(int enable)
{
    rt_base_t miso_pad = s_spi_iomux_mode ? S31_SPI2_IOMUX_MISO : S31_SPI2_MISO_PAD;
    rt_uint32_t off = s31_iomux_off[miso_pad];
    rt_uint32_t v;

    if (off == 0xFFFFFFFFu) {
        return;
    }
    v = S31_REG32(S31_IO_MUX_BASE + off);
    if (enable) {
        v |= IOMUX_FUN_PD;
    } else {
        v &= ~IOMUX_FUN_PD;
    }
    S31_REG32(S31_IO_MUX_BASE + off) = v;
}

/* 打开/关闭某个焊盘的输入缓冲（FUN_IE）。
 * ⚠️ 环回要靠这个：`spi_mux_out()` 把焊盘配成输出时会**清掉 IE**，
 *    于是同一根焊盘自己的输入通路是断的，环回必然读到 0（踩过）。*/
static void spi_set_pad_ie(rt_base_t pad, int enable)
{
    rt_uint32_t off, v;

    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    off = s31_iomux_off[pad];
    if (off == 0xFFFFFFFFu) {
        return;
    }
    v = S31_REG32(S31_IO_MUX_BASE + off);
    if (enable) {
        v |= IOMUX_FUN_IE;
    } else {
        v &= ~IOMUX_FUN_IE;
    }
    S31_REG32(S31_IO_MUX_BASE + off) = v;
}

/* ---- RT-Thread ops ------------------------------------------------------ */

/* 注意：本版 RT-Thread 的 rt_spi_transfer_message() 会**自己遍历 message 链**、
 * 逐个调用 xfer，所以这里只处理当前这一条（别再跟 next，否则会重复发）。*/
static rt_ssize_t s31_spi_xfer(struct rt_spi_device *device, struct rt_spi_message *message)
{
    struct s31_spi_hw *spi = &s_spi2_hw;

    if (message->cs_take) {
        spi_cs_assert(spi);
    }
    if (message->length) {
        if (message->length > 64u) {              /* PIO 一次最多 64 字节 */
            rt_set_errno(-RT_EINVAL);
            spi_cs_release(spi);
            return 0;
        }
        spi_hw_transfer(spi, (const rt_uint8_t *)message->send_buf,
                        (rt_uint8_t *)message->recv_buf, message->length);
    }
    if (message->cs_release) {
        spi_cs_release(spi);
    }
    return (rt_ssize_t)message->length;
}

static const struct rt_spi_ops s31_spi_ops =
{
    s31_spi_configure,
    s31_spi_xfer,
};

/* QSPI 用同一套寄存器，但把消息当 rt_qspi_message 解释 */
static rt_ssize_t s31_qspi_xfer(struct rt_spi_device *device, struct rt_spi_message *message)
{
    struct s31_spi_hw *spi = &s_spi2_hw;
    struct rt_qspi_message *q = (struct rt_qspi_message *)message;
    rt_size_t len = message->length;

    if (len > 64u) {
        rt_set_errno(-RT_EINVAL);
        return 0;
    }
    if (message->cs_take) {
        spi_cs_assert(spi);
    }
    if (len) {
        qspi_hw_transfer(spi, q);
    }
    if (message->cs_release) {
        spi_cs_release(spi);
    }
    return len;
}

static const struct rt_spi_ops s31_qspi_ops =
{
    s31_spi_configure,
    s31_qspi_xfer,
};

/* ---- 初始化 ------------------------------------------------------------- */

static int s31_spi_init(void)
{
    rt_uint32_t ctrl;
    struct rt_qspi_configuration qcfg;
    static struct rt_spi_device  s_flash_dev;
    static struct rt_qspi_device s_qflash_dev;

    s_spi2_hw.base   = S31_SPI2_BASE;
    s_spi2_hw.src_hz = 40000000u;           /* GPSPI2 默认源 = XTAL 40MHz */

    /* 时钟门控（bit0 SYS_CLK_EN / bit1 APB_CLK_EN 复位默认已是 1，这里再确认）*/
    ctrl = S31_REG32(CLKRST_GPSPI2_REG);
    ctrl |= (1u << 0) | (1u << 1) | (1u << 6) | (1u << 23);   /* SYS/APB/HS/MST 使能 */
    ctrl &= ~(3u << 4);                                        /* 源 = XTAL */
    ctrl &= ~(0xFFu << 7);                                     /* hs_div = 1 */
    ctrl &= ~(0xFFu << 15);                                    /* mst_div = 1 */
    S31_REG32(CLKRST_GPSPI2_REG) = ctrl;

    /* 引脚：sfud 工程那套接线（SCK=43 MOSI=44 MISO=45 CS=46）*/
    spi_set_pins_matrix(S31_SPI2_SCK_PAD, S31_SPI2_MOSI_PAD,
                        S31_SPI2_MISO_PAD, S31_SPI2_CS_PAD);

    /* 复位一次：RST_EN 脉冲 */
    S31_REG32(CLKRST_GPSPI2_REG) |= (1u << 2);
    S31_REG32(CLKRST_GPSPI2_REG) &= ~(1u << 2);

    /* 两条总线：spi2（普通 SPI）与 qspi2（QSPI 消息），共用同一组寄存器 */
    rt_spi_bus_register(&s_spi2_bus, "spi2", &s31_spi_ops);
    rt_qspi_bus_register(&s_qspi2_bus, "qspi2", &s31_qspi_ops);

    /* CS 由驱动自己拉，所以 attach 时 cs_pin 传 PIN_NONE */
    rt_spi_bus_attach_device_cspin(&s_flash_dev, "flash0", "spi2", PIN_NONE, RT_NULL);
    rt_spi_bus_attach_device_cspin(&s_qflash_dev.parent, "qflash0", "qspi2",
                                   PIN_NONE, RT_NULL);

    /* QSPI 设备参数：8MB flash、最大 4 线 */
    rt_memset(&qcfg, 0, sizeof(qcfg));
    qcfg.parent.max_hz   = 20000000u;
    qcfg.parent.data_width = 8;
    qcfg.parent.mode     = RT_SPI_MODE_0 | RT_SPI_MSB;
    qcfg.medium_size     = 8u * 1024u * 1024u;
    qcfg.qspi_dl_width   = 4;
    rt_qspi_configure(&s_qflash_dev, &qcfg);
    /* ⚠️ 再把同一份配置写进"内嵌的 rt_spi_device.config"：
     * rt_qspi_transfer_message()/rt_spi_bus_configure() 走的是**这一份**
     * （见 s31_spi_configure 里的注释），不同步的话 QSPI 配置等于白设。*/
    s_qflash_dev.parent.config = qcfg.parent;

    rt_kprintf("[spi] spi2/qspi2 ready: SCK=GPIO43 MOSI=GPIO44 MISO=GPIO45 CS=GPIO46 "
               "(src 40MHz, PIO max 64B)\n");
    /* 自证：CS 焊盘的 OUT_SEL 必须是 SPI2_CS0(62) —— 开机就把这个数打出来，
     * 免得再出现"片选根本没接上、事务白跑"这种看不见的错（踩过）。*/
    rt_kprintf("[spi] 自检 CS: GPIO%d OUT_SEL=%u(应=%u)  SCK=%u MOSI=%u  MISO_IN54=%u\n",
               (int)S31_SPI2_CS_PAD,
               (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(S31_SPI2_CS_PAD)),
               (unsigned)SPI2_CS0_SIG,
               (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(S31_SPI2_SCK_PAD)),
               (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(S31_SPI2_MOSI_PAD)),
               (unsigned)S31_REG32(GPIO_FUNC_IN_SEL(SPI2_Q_SIG)));
    return 0;
}
INIT_DEVICE_EXPORT(s31_spi_init);
