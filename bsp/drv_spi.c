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
 *
 * 🚨 CS 是**软件驱动的普通 GPIO**（不是外设的 CS0 信号），原因见
 *   `spi_set_pins_matrix()` 上面那段长注释 —— 简言之：外设 CS0 在两次事务
 *   之间不会把线抬起来，flash 就丢了"命令边界"，现象是**只有第一条事务能通**。
 *   这一条 2026-09-25 花了整整一轮才挖出来，别再改回外设片选。
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
/* DMA_CONF 位号（以 IDF `soc/esp32s31/register/soc/spi_struct.h` + `spi_ll.h` 为准）：
 *   bit27 dma_rx_ena        = spi_ll_dma_rx_enable
 *   bit28 dma_tx_ena        = spi_ll_dma_tx_enable
 *   bit29 rx_afifo_rst      = spi_ll_dma_rx_fifo_reset（**收**侧异步 FIFO；PIO 也靠它清收侧）
 *   bit30 buf_afifo_rst     = spi_ll_cpu_tx_fifo_reset（CPU/PIO 的**发** FIFO）
 *   bit31 dma_afifo_rst     = spi_ll_dma_tx_fifo_reset（**DMA** 的发 FIFO）
 * ⚠️ 30 和 31 是两个不同的发 FIFO：PIO 用 30、DMA 用 31，别混。
 *    （本 port 一度把 29 当成"不相干的位"改掉，反而把收侧复位弄丢了。）*/
#define SPI_DMA_RX_ENA        (1u << 27)
#define SPI_DMA_TX_ENA        (1u << 28)
#define SPI_RX_AFIFO_RST      (1u << 29)
#define SPI_CPU_TX_FIFO_RST   (1u << 30)
#define SPI_DMA_TX_FIFO_RST   (1u << 31)
/* DMA_INT */
#define SPI_TRANS_DONE        (1u << 12)
/* 中断标志：RAW(0x3C) 是只读状态，清位必须写 CLR(0x38) */
#define SPI_DMA_INT_CLR       (SPI_DMA_INT_RAW - 4u)
/* 收发 FIFO 溢出/空的错误标志（INFIFO_FULL_ERR / OUTFIFO_EMPTY_ERR），DMA 收发前清一下 */
#define SPI_DMA_INFIFO_FULL_ERR_CLR   (1u << 6)
#define SPI_DMA_OUTFIFO_EMPTY_ERR_CLR (1u << 5)
/* 一次事务的**硬件上限**：SPI_MS_DATA_BITLEN 是 18 位 ⇒ 262143 bit = **32767 字节** */
#define S31_SPI_DMA_MAX_XFER  32767u

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
static void spi_cs_pad_oe(rt_base_t pad, int enable);
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
    /* 🚨 这根焊盘现在是**输入**（MISO），必须先把输出使能关掉：
     *    它可能刚当过别的角色（LCD 档的 CS 就是 GPIO45），那时 OE 是开的、
     *    OUT_SEL 指向 GPIO_OUT —— 不收手的话它会一直驱动，和从机的 MISO 抢线。
     *    （实测：`lcd fill` 之后切回 matrix 档，GPIO45 的 OUT_SEL 还是 256。）*/
    spi_cs_pad_oe(pad, 0);

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

/* ---- CS：软件驱动的普通 GPIO（原因见 spi_set_pins_matrix 上面的长注释）----
 * 用的是 GPIO_OUT1/ENABLE1（GPIO32~63 在第二组寄存器里，掩码右移 32）。*/
#define GPIO_OUT_W1TS      (S31_GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC      (S31_GPIO_BASE + 0x0Cu)
#define GPIO_OUT1_W1TS     (S31_GPIO_BASE + 0x14u)
#define GPIO_OUT1_W1TC     (S31_GPIO_BASE + 0x18u)
#define GPIO_ENABLE_W1TS   (S31_GPIO_BASE + 0x38u)
#define GPIO_ENABLE_W1TC   (S31_GPIO_BASE + 0x3Cu)
#define GPIO_ENABLE1_W1TS  (S31_GPIO_BASE + 0x44u)
#define GPIO_ENABLE1_W1TC  (S31_GPIO_BASE + 0x48u)

/* 当前哪个焊盘当 CS（默认 46；切到 LCD 档时是 45）*/
static rt_base_t s_spi_cs_pad = S31_SPI2_CS_PAD;

static void spi_cs_pad_oe(rt_base_t pad, int enable)
{
    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    if (pad < 32) {
        S31_REG32(enable ? GPIO_ENABLE_W1TS : GPIO_ENABLE_W1TC) = 1u << pad;
    } else {
        S31_REG32(enable ? GPIO_ENABLE1_W1TS : GPIO_ENABLE1_W1TC) = 1u << (pad - 32);
    }
}

static void spi_cs_pad_drive(rt_base_t pad, int high)
{
    if (pad < 0 || pad >= S31_GPIO_PIN_COUNT) {
        return;
    }
    if (pad < 32) {
        S31_REG32(high ? GPIO_OUT_W1TS : GPIO_OUT_W1TC) = 1u << pad;
    } else {
        S31_REG32(high ? GPIO_OUT1_W1TS : GPIO_OUT1_W1TC) = 1u << (pad - 32);
    }
}

/* 把一个焊盘配成"CS"：输出源 = GPIO_OUT、打开输出使能、空闲拉高 */
static void spi_cs_pad_setup(rt_base_t pad)
{
    rt_uint32_t off, v;

    s_spi_cs_pad = pad;
    spi_cs_pad_drive(pad, 1);                       /* 先给高电平值，避免接管瞬间拉低 */
    spi_cs_pad_oe(pad, 1);                          /* 🚨 少了这句焊盘就是高阻（踩过）*/

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
    S31_REG32(GPIO_FUNC_OUT_SEL(pad)) = SIG_GPIO_OUT_REG;   /* 输出源取 GPIO_OUT 寄存器 */
}

/* 用 GPIO matrix 接线（默认就是 sfud 工程那套：43/44/45/46）
 *
 * 🚨 CS 单独处理：**改成软件驱动的普通 GPIO**，不再用外设的 CS0 信号。
 *
 * 为什么（2026-09-25 实测定论，三条证据）：
 *   ① 软件位翻转读 0x9F，**连读 5 次全对**（`sf bb`）⇒ flash/接线/供电没问题；
 *   ② 环回（`spi_loop`）连跑 3 次全过 ⇒ 时钟/FIFO/收发通路没问题；
 *   ③ 但走外设 CS0 时，**只有第一条事务通，之后永远读回全 0**；
 *      而回读 `GPIO_IN1` 发现 CS 焊盘在空闲时是 **低**。
 *   ⇒ 根因：外设 CS0 信号只在 USR 期间有效，**事务之间那个焊盘没有把线拉高**
 *     （既没被驱动、模块上也没有上拉）→ flash 看不到"CS 抬起"这个命令边界
 *     → 它把第二条 0x9F 当成上一条命令的续传数据吞掉 → 读回全 0。
 *     软复位一次能再通一次，也正是因为复位瞬间焊盘松开、线被抬起来重新同步了。
 *
 *   顺带记一笔：原来"手动 GPIO 拉 CS"之所以没生效，是因为**没有打开输出使能**
 *   （`GPIO_ENABLE1` 对应位是 0），焊盘一直是高阻。现在两者都补上了。
 *   时序上反而更稳：CS 的建立/保持完全由我们掌握（写 GPIO → 若干寄存器操作 → USR），
 *   比外设 CS_SETUP/CS_HOLD 更直观，而且**空闲时永远是确定的高电平**。*/
static void spi_set_pins_matrix(rt_base_t sck, rt_base_t mosi, rt_base_t miso, rt_base_t cs)
{
    spi_mux_out(sck,  SPI2_CK_SIG);          /* CLK 输出 */
    spi_mux_out(mosi, SPI2_D_SIG);           /* MOSI 输出 */
    spi_mux_in(miso,  SPI2_Q_SIG);           /* MISO 输入 */
    spi_cs_pad_setup(cs);                    /* CS = 软件驱动的 GPIO（见上）*/
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
    spi_cs_pad_drive(pad, 1);                /* 先抬成高电平，别把线按在地上 */
    spi_cs_pad_oe(pad, 0);                   /* 再关输出使能 → 高阻 */
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

/* ---- CS：软件驱动（低有效）--------------------------------------------- */
/* 为什么不用外设的 CS0 + CS_KEEP_ACTIVE：见 spi_set_pins_matrix 上面的长注释 ——
 * 外设 CS0 在两次事务之间不会把线抬起来，flash 就丢了命令边界（实测"只有第一条
 * 事务能通"）。这里改成明确地"拉低/拉高"，空闲永远是高电平。*/
static void spi_cs_assert(struct s31_spi_hw *spi)
{
    (void)spi;
    spi_cs_pad_drive(s_spi_cs_pad, 0);
}

static void spi_cs_release(struct s31_spi_hw *spi)
{
    (void)spi;
    spi_cs_pad_drive(s_spi_cs_pad, 1);
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

/* ---- 底层：一次 full-duplex -------------------------------------------- */

/* PIO 一次最多能搬多少：收发各一个 16 字 FIFO = 64 字节。
 * 超过这个长度的传输走 DMA（见文件上方的 DMA 段）。*/
#define S31_SPI_PIO_MAX      64u

static void spi_start(struct s31_spi_hw *spi)
{
    spi_wr(spi, SPI_DMA_INT_CLR, 0xFFFFFFFFu);        /* 清 TRANS_DONE（写 RAW 是清不掉的）*/
    spi_wr(spi, SPI_CMD, SPI_CMD_UPDATE);
    while (spi_rd(spi, SPI_CMD) & SPI_CMD_UPDATE) {
    }
    spi_wr(spi, SPI_CMD, SPI_CMD_USR);
    while (spi_rd(spi, SPI_CMD) & SPI_CMD_USR) {
    }
    spi_wr(spi, SPI_DMA_INT_CLR, 0xFFFFFFFFu);
}

static void spi_hw_transfer(struct s31_spi_hw *spi, const rt_uint8_t *tx,
                            rt_uint8_t *rx, rt_uint32_t len)
{
    rt_uint32_t i, k, w;

    /* FIFO 复位（长度寄存器每次都要重设） */
    spi_wr(spi, SPI_DMA_CONF, SPI_RX_AFIFO_RST | SPI_CPU_TX_FIFO_RST);   /* PIO：收侧 + CPU 发 FIFO */
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

    spi_wr(spi, SPI_DMA_CONF, SPI_RX_AFIFO_RST | SPI_CPU_TX_FIFO_RST);   /* PIO：收侧 + CPU 发 FIFO */
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

/* ---- DMA：AXI PDMA（让一次事务能搬任意长度）------------------------------
 *
 * 为什么需要：PIO 通路的收发各只有一个 **16 字（64 字节）FIFO**，所以一次
 * `rt_spi_transfer()` 最多 64 字节（超了驱动 `rt_set_errno(-RT_EINVAL)` 并返回 0）。
 * 要一次搬 4 KB 就必须走 DMA。
 *
 * 事实来源（都核对过 IDF 的 esp32s31 头文件，别凭记忆改）：
 *   · SPI2 挂在 **AXI PDMA** 上：`SOC_GDMA_TRIG_PERIPH_SPI2 = 1`，
 *     `SOC_GDMA_TRIG_PERIPH_SPI2_BUS = AXI`（esp_hal_dma/esp32s31/include/hal/gdma_channel.h）
 *   · AXI DMA 基址 **0x20348000**（soc/reg_base.h 的 DR_REG_AXI_DMA_BASE）
 *   · 通道布局：IN 通道从 +0x000 起、OUT 从 +0x138 起，**每通道 0x68 字节**；
 *     通道内：INT_RAW +0x00（bit1 = suc_eof）、INT_CLR +0x0C、CONF0 +0x10
 *     （bit0 = rst、bit2 = mem_trans_en）、LINK1 +0x20（IN bit2 / OUT bit1 = start）、
 *     LINK2 +0x24（描述符地址）、PERI_SEL +0x44（[5:0] = 外设号，SPI2 = 1）
 *   · 描述符 = **16 字节、8 字节对齐**（AXI 的 `GDMA_LL_AXI_DESC_ALIGNMENT = 8`；
 *     IDF 的 spi_common_internal.h 在 AXI 总线这条分支上就是选 align8 那份）：
 *       dw0[11:0]=size、[23:12]=length、[30]=suc_eof、[31]=owner
 *     填法照 IDF：size = length = 本次字节数，owner = 1（DMA 持有），最后一条 suc_eof = 1
 *   · SPI 侧：DMA_CONF(0x30) bit27 = dma_rx_ena、bit28 = dma_tx_ena、
 *     bit30 = buf_afifo_rst、bit31 = dma_afifo_rst；
 *     中断**清位要写 DMA_INT_CLR(0x38)** —— 写 DMA_INT_RAW 是清不掉的（它是只读状态）
 *
 * ⚠️ 缓冲必须是**内部 RAM**：本 port 的内部 RAM 在 0x2F000000 一带，
 *    cache 只覆盖外部存储窗口（0x40000000 起，PSRAM 0x50000000 就在里面）。
 *    所以内部 RAM 缓冲**不需要任何 cache 维护**；PSRAM 缓冲会被这里挡回去走
 *    PIO 分块（见 s31_spi_xfer），**不会**让 DMA 去碰带 cache 的内存 ——
 *    这样就没机会踩"cache 行没写回/没失效"那类偶发数据错。
 *-------------------------------------------------------------------------*/
#define S31_AXI_DMA_BASE     0x20348000u
#define DMA_MISC_CONF        (S31_AXI_DMA_BASE + 0x2A8u)   /* bit4 = clk_en */
#define DMA_IN_CH(n)         (S31_AXI_DMA_BASE + 0x000u + 0x68u * (n))
#define DMA_OUT_CH(n)        (S31_AXI_DMA_BASE + 0x138u + 0x68u * (n))
#define DMA_INT_RAW(c)       ((c) + 0x00u)
#define DMA_INT_CLR(c)       ((c) + 0x0Cu)
#define DMA_CONF0(c)         ((c) + 0x10u)
#define DMA_LINK1(c)         ((c) + 0x20u)
#define DMA_LINK2(c)         ((c) + 0x24u)
#define DMA_PERI_SEL(c)      ((c) + 0x44u)

#define DMA_INT_SUC_EOF      (1u << 1)
#define DMA_INT_ERR_EOF      (1u << 2)
#define DMA_CONF0_RST        (1u << 0)
#define DMA_LINK1_START_IN   (1u << 2)     /* IN 通道的 start 在 bit2 */
#define DMA_LINK1_START_OUT  (1u << 1)     /* OUT 通道的 start 在 bit1 */
#define DMA_PERI_SPI2        1u            /* SOC_GDMA_TRIG_PERIPH_SPI2 */
#define DMA_MISC_CLK_EN      (1u << 4)
/* AXI DMA 要知道"哪些地址段合法"，不配它可能直接拒绝访问。
 * 取值照 IDF 的 `axi_dma_ll_set_default_memory_range()`：
 *   内部 SRAM 0x2F000000..0x2F07FFFF、外部（PSRAM）0x40000000..0x53FFFFFF。
 * 顺带印证了两件事：① S31 的内部 RAM 就是 0x2F000000 起 512KB；
 * ② DMA 直接用它，**不用做 cache 维护**（PSRAM 那段才是"外部/cache"的）。*/
#define DMA_INTR_MEM_START   (S31_AXI_DMA_BASE + 0x27Cu)
#define DMA_INTR_MEM_END     (S31_AXI_DMA_BASE + 0x280u)
#define DMA_EXTR_MEM_START   (S31_AXI_DMA_BASE + 0x284u)
#define DMA_EXTR_MEM_END     (S31_AXI_DMA_BASE + 0x288u)

/* 描述符：16 字节、8 字节对齐（AXI DMA 要求）*/
typedef struct {
    volatile rt_uint32_t dw0;
    volatile rt_uint32_t buf;
    volatile rt_uint32_t next;
    volatile rt_uint32_t rsv;
} __attribute__((aligned(8))) s31_dma_desc_t;

#define DMA_DESC_MAX         16u                       /* 16 × 4092 ≈ 64 KB 一次搬完 */
#define DMA_DESC_MAX_LEN     4092u                     /* size 字段 12 位，留 4 字节对齐余量 */

static s31_dma_desc_t s_dma_rx_desc[DMA_DESC_MAX] __attribute__((aligned(8)));
static s31_dma_desc_t s_dma_tx_desc[DMA_DESC_MAX] __attribute__((aligned(8)));

/* 一次 DMA 事务最多搬多少（描述符条数上限）*/
#define S31_SPI_DMA_MAX      (DMA_DESC_MAX * DMA_DESC_MAX_LEN)

/* 按缓冲区建描述符链，返回用掉几条；len 超过上限返回 0 */
static rt_uint32_t dma_build_chain(s31_dma_desc_t *desc, rt_uint32_t addr, rt_uint32_t len)
{
    rt_uint32_t n = 0;

    if (len == 0u || len > S31_SPI_DMA_MAX) {
        return 0u;
    }
    while (len != 0u) {
        rt_uint32_t chunk = (len > DMA_DESC_MAX_LEN) ? DMA_DESC_MAX_LEN : len;

        if (n >= DMA_DESC_MAX) {
            return 0u;
        }
        desc[n].dw0  = (chunk & 0xFFFu) | ((chunk & 0xFFFu) << 12) |
                       (1u << 31);                    /* owner = DMA */
        desc[n].buf  = addr;
        desc[n].next = (rt_uint32_t)&desc[n + 1u];
        n++;
        addr += chunk;
        len  -= chunk;
    }
    desc[n - 1u].next   = 0u;
    desc[n - 1u].dw0   |= (1u << 30);                 /* 最后一条：suc_eof */
    return n;
}

/* 等一个 DMA 通道跑完（只看 suc_eof/err_eof，带超时 —— 本工程不吃无上限忙等）*/
static int dma_wait_done(rt_uint32_t ch)
{
    rt_uint64_t t0 = s31_systimer_get_ticks();
    rt_uint64_t limit = (rt_uint64_t)(S31_SYSTIMER_HZ / 1000u) * 2000u;   /* 2 秒 */

    while ((rt_uint64_t)(s31_systimer_get_ticks() - t0) < limit) {
        rt_uint32_t raw = S31_REG32(DMA_INT_RAW(ch));

        if (raw & DMA_INT_ERR_EOF) {
            return -2;
        }
        if (raw & DMA_INT_SUC_EOF) {
            return 0;
        }
    }
    return -1;                                        /* 超时 */
}

/* 把通道恢复到"可再次下发"的状态 */
static void dma_channel_cleanup(rt_uint32_t ch)
{
    S31_REG32(DMA_INT_CLR(ch)) = 0xFFFFFFFFu;
    S31_REG32(DMA_CONF0(ch)) |= DMA_CONF0_RST;
    S31_REG32(DMA_CONF0(ch)) &= ~DMA_CONF0_RST;
}

/* 一次性把 AXI PDMA 唤醒并配上（照 IDF `gdma_ll_enable_bus_clock/reset_register`
 * + `axi_dma_ll_set_default_memory_range` 那一套）：
 *   · `HP_SYS_CLKRST.axi_pdma_ctrl0`(0x20587078)：bit0 = sys_clk_en（复位默认已是 1）、
 *     bit1 = rst_en（脉冲 1→0 复位一次）；
 *   · `AXI_DMA.misc_conf`(+0x2A8) bit4 = clk_en（寄存器时钟强制常开）；
 *   · 合法地址段：内部 SRAM 0x2F000000~0x2F07FFFF、外部 0x40000000~0x53FFFFFF。
 * 顺带印证了 §DMA 注释里那句"内部 RAM 不用做 cache 维护"：DMA 就是直接访问这段地址，
 * 而 S31 的 cache 只覆盖 0x40000000 起的外部窗口（`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`
 * 在 esp32s31 未定义）。*/
#define HP_SYS_CLKRST_AXI_PDMA_CTRL0  (S31_HP_SYS_CLKRST_BASE + 0x78u)

static void dma_hw_prepare_once(void)
{
    static rt_bool_t done = RT_FALSE;
    rt_uint32_t v;

    if (done) {
        return;
    }
    done = RT_TRUE;

    v  = S31_REG32(HP_SYS_CLKRST_AXI_PDMA_CTRL0);
    v |= (1u << 0);                                   /* sys_clk_en */
    S31_REG32(HP_SYS_CLKRST_AXI_PDMA_CTRL0) = v;
    S31_REG32(HP_SYS_CLKRST_AXI_PDMA_CTRL0) = v | (1u << 1);    /* rst_en = 1 */
    S31_REG32(HP_SYS_CLKRST_AXI_PDMA_CTRL0) = v;                /* rst_en = 0 */

    S31_REG32(DMA_MISC_CONF) |= DMA_MISC_CLK_EN;      /* 寄存器时钟常开 */

    S31_REG32(DMA_INTR_MEM_START) = 0x2F000000u;
    S31_REG32(DMA_INTR_MEM_END)   = 0x2F07FFFFu;
    S31_REG32(DMA_EXTR_MEM_START) = 0x40000000u;
    S31_REG32(DMA_EXTR_MEM_END)   = 0x53FFFFFFu;
}

/* 启动一条通道：连外设 → 装描述符 → start */
static void dma_channel_start(rt_uint32_t ch, s31_dma_desc_t *desc, rt_uint32_t start_bit)
{
    rt_uint32_t c0;

    dma_hw_prepare_once();
    S31_REG32(DMA_PERI_SEL(ch)) = DMA_PERI_SPI2;

    /* CONF0：bit0=通道复位、bit2=mem_trans_en(外设模式要 0)、bit[6:4]=突发长度、
     *        bit9/bit10=描述符突发。照 IDF 设成"描述符突发使能 + 32 字节数据突发"。*/
    c0  = S31_REG32(DMA_CONF0(ch));
    c0 &= ~((7u << 4) | (1u << 2));
    c0 |= (2u << 4);                                  /* ctz(32)-3 = 2 ⇒ 32 字节突发 */
    c0 |= (1u << 9);                                  /* indscr_burst_en */
    c0 |= (1u << 10);                                 /* outdscr_burst_en */
    S31_REG32(DMA_CONF0(ch)) = c0;

    S31_REG32(DMA_LINK2(ch)) = (rt_uint32_t)desc;
    S31_REG32(DMA_LINK1(ch)) = start_bit;
}

/* DMA 单次事务：tx/rx 二选一（本工程用到的都是单向数据相位）。
 * 返回 0 = 成功；负数 = 失败（调用方回退到分块 PIO）。
 *
 * 几个必须守住的约束（都来自 IDF 的 S31 头/LL，别改）：
 *   · **一次事务最多 32767 字节**（`SPI_MS_DATA_BITLEN` 18 位 ⇒ 262143 bit）；
 *   · 收方向要求长度是 **4 的倍数**：IDF 的 RX 描述符 `size` 会向上取整到 4
 *     （AXI 写是整字突发），不整除就可能多写 3 字节到缓冲区外 —— 干脆挡回去走 PIO；
 *   · **先把 DMA 武装好再启动 SPI**（IDF `s_spi_dma_prepare_data` 就是这个顺序）；
 *   · 等 **SPI 的 TRANS_DONE（bit12）** 作为完成判据（IDF 主机侧只等它，
 *     从不注册 GDMA 回调），DMA 的 `suc_eof` 只当二次确认。*/
static int spi_dma_transfer(struct s31_spi_hw *spi, const rt_uint8_t *tx,
                            rt_uint8_t *rx, rt_uint32_t len)
{
    rt_uint32_t user = 0, dma_bits = 0;
    rt_uint32_t rx_n = 0, tx_n = 0;
    rt_uint32_t ch;
    int rc = 0;

    if (len == 0u || len > S31_SPI_DMA_MAX_XFER) {
        return -3;
    }
    if (rx != RT_NULL && (len & 3u) != 0u) {          /* 见上：收方向要 4 字节整除 */
        return -3;
    }

    dma_hw_prepare_once();

    if (rx != RT_NULL) {
        rx_n = dma_build_chain(s_dma_rx_desc, (rt_uint32_t)rx, len);
        if (rx_n == 0u) {
            return -3;
        }
        user |= SPI_USER_USR_MISO;
        dma_bits |= SPI_DMA_RX_ENA;
    }
    if (tx != RT_NULL) {
        tx_n = dma_build_chain(s_dma_tx_desc, (rt_uint32_t)tx, len);
        if (tx_n == 0u) {
            return -3;
        }
        user |= SPI_USER_USR_MOSI;
        dma_bits |= SPI_DMA_TX_ENA;
    }

    /* FIFO 复位 + 清溢出/空标志（IDF 的 spi_hal_hw_prepare_rx/tx 就是这三步）*/
    spi_wr(spi, SPI_DMA_CONF, SPI_RX_AFIFO_RST | SPI_DMA_TX_FIFO_RST);
    spi_wr(spi, SPI_DMA_CONF, 0);
    spi_wr(spi, SPI_DMA_INT_CLR,
           (rx_n != 0u ? SPI_DMA_INFIFO_FULL_ERR_CLR : 0u) |
           (tx_n != 0u ? SPI_DMA_OUTFIFO_EMPTY_ERR_CLR : 0u) |
           SPI_TRANS_DONE);

    /* 先把 DMA 武装好，再让 SPI 开始打时钟 —— 反了的话前几个字节没人接 */
    if (rx_n != 0u) {
        ch = DMA_IN_CH(0u);
        dma_channel_cleanup(ch);
        dma_channel_start(ch, s_dma_rx_desc, DMA_LINK1_START_IN);
    }
    if (tx_n != 0u) {
        ch = DMA_OUT_CH(0u);
        dma_channel_cleanup(ch);
        dma_channel_start(ch, s_dma_tx_desc, DMA_LINK1_START_OUT);
    }

    spi_wr(spi, SPI_USER, user | SPI_USER_DOUTDIN | SPI_USER_CS_HOLD | SPI_USER_CS_SETUP);
    spi_wr(spi, SPI_MS_DLEN, len * 8u - 1u);
    spi_wr(spi, SPI_DMA_CONF, dma_bits);
    spi_start(spi);                                   /* 起 USR 并等它自清 */

    /* 完成判据：SPI 的 TRANS_DONE（IDF 主机侧唯一等的标志），带超时 */
    {
        rt_uint64_t t0 = s31_systimer_get_ticks();
        rt_uint64_t limit = (rt_uint64_t)(S31_SYSTIMER_HZ / 1000u) * 2000u;   /* 2 秒 */
        int done = 0;

        while ((rt_uint64_t)(s31_systimer_get_ticks() - t0) < limit) {
            if (spi_rd(spi, SPI_DMA_INT_RAW) & SPI_TRANS_DONE) {
                done = 1;
                break;
            }
        }
        if (!done) {
            rc = -1;
        }
    }

    /* 二次确认：DMA 侧把最后一个描述符收完了没（有 suc_eof 才算真搬完）*/
    if (rx_n != 0u) {
        int rc2 = dma_wait_done(DMA_IN_CH(0u));
        if (rc == 0) {
            rc = rc2;
        }
        dma_channel_cleanup(DMA_IN_CH(0u));
    }
    if (tx_n != 0u) {
        int rc2 = dma_wait_done(DMA_OUT_CH(0u));
        if (rc == 0) {
            rc = rc2;
        }
        dma_channel_cleanup(DMA_OUT_CH(0u));
    }

    spi_wr(spi, SPI_DMA_CONF, 0);                     /* 关掉 DMA，别影响后面的 PIO 传输 */
    return rc;
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
        spi_cs_pad_setup(LCD_CS_PAD);                     /* 45 = 软件 CS */
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

/* 一个缓冲区能不能直接交给 DMA：**必须在内部 RAM**。
 *
 * cache 只覆盖外部存储窗口（0x40000000 起 —— PSRAM 的 0x50000000 就在里面），
 * 所以内部 RAM（0x2F000000 一带）的读写**不经过 cache**，DMA 碰它不需要任何维护；
 * 而 PSRAM 缓冲直接给 DMA 就要处理"写回/失效 + 64 字节行对齐"，一旦漏了就变成
 * 偶发数据错（最难查的那种）。这里索性**一律挡回去走分块 PIO** ——
 * 慢一点，但绝不会错。*/
static rt_bool_t s31_buf_dma_ok(const void *p, rt_uint32_t len)
{
    rt_uint32_t a = (rt_uint32_t)p;

    return (a != 0u) && (a < 0x40000000u) && ((a + len) < 0x40000000u);
}

/* 分块 PIO：缓冲区在 PSRAM、或长度超过描述符上限时的保底通路。
 * 一次搬 ≤64 字节，中间 CS 一直压着（对从机来说仍是一次连续片选）。*/
static void s31_spi_xfer_pio_chunked(struct s31_spi_hw *spi, const rt_uint8_t *tx,
                                     rt_uint8_t *rx, rt_uint32_t len)
{
    rt_uint32_t off;

    for (off = 0; off < len; off += S31_SPI_PIO_MAX) {
        rt_uint32_t n = len - off;

        if (n > S31_SPI_PIO_MAX) {
            n = S31_SPI_PIO_MAX;
        }
        spi_hw_transfer(spi, (tx != RT_NULL) ? (tx + off) : RT_NULL,
                        (rx != RT_NULL) ? (rx + off) : RT_NULL, n);
    }
}

/* 注意：本版 RT-Thread 的 rt_spi_transfer_message() 会**自己遍历 message 链**、
 * 逐个调用 xfer，所以这里只处理当前这一条（别再跟 next，否则会重复发）。*/
static rt_ssize_t s31_spi_xfer(struct rt_spi_device *device, struct rt_spi_message *message)
{
    struct s31_spi_hw *spi = &s_spi2_hw;
    rt_uint32_t len = message->length;
    const rt_uint8_t *tx = (const rt_uint8_t *)message->send_buf;
    rt_uint8_t *rx = (rt_uint8_t *)message->recv_buf;

    if (message->cs_take) {
        spi_cs_assert(spi);
    }
    if (len != 0u) {
        if (len <= S31_SPI_PIO_MAX) {
            spi_hw_transfer(spi, tx, rx, len);                /* 短事务：PIO 最省事 */
        } else if (len <= S31_SPI_DMA_MAX &&
                   s31_buf_dma_ok(tx, len) && s31_buf_dma_ok(rx, len)) {
            /* 长事务 + 缓冲区在内部 RAM：一次 DMA 搬完（这才是"任意长度"）*/
            if (spi_dma_transfer(spi, tx, rx, len) != 0) {
                s31_spi_xfer_pio_chunked(spi, tx, rx, len);   /* DMA 没跑成 → 退 PIO */
            }
        } else {
            s31_spi_xfer_pio_chunked(spi, tx, rx, len);
        }
    }
    if (message->cs_release) {
        spi_cs_release(spi);
    }
    return (rt_ssize_t)len;
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
    /* 自证：CS 焊盘的 OUT_SEL 必须是 256(GPIO_OUT)、**输出使能必须是 1** ——
     * 软件片选的两个关键条件，缺一个就是"片选永不生效"（两个都踩过）。*/
    rt_kprintf("[spi] 自检 CS: GPIO%d OUT_SEL=%u(应=%u) OE=%u(应=1)  SCK=%u MOSI=%u  MISO_IN54=%u\n",
               (int)S31_SPI2_CS_PAD,
               (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(S31_SPI2_CS_PAD)),
               (unsigned)SIG_GPIO_OUT_REG,
               (unsigned)((S31_REG32(S31_GPIO_BASE + 0x40u) >> (S31_SPI2_CS_PAD - 32)) & 1u),
               (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(S31_SPI2_SCK_PAD)),
               (unsigned)S31_REG32(GPIO_FUNC_OUT_SEL(S31_SPI2_MOSI_PAD)),
               (unsigned)S31_REG32(GPIO_FUNC_IN_SEL(SPI2_Q_SIG)));
    return 0;
}
INIT_DEVICE_EXPORT(s31_spi_init);
