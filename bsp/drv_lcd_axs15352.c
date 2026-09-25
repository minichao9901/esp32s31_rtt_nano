/*===========================================================================
 * drv_lcd_axs15352.c -- 外接 SPI 屏（天马 2.01" / AXS15352，240x296）挂到 SPI 框架上
 *
 * 这是把 IDF 工程 `projects\spi_lcd_axs15352` 那一套搬到本 port（不依赖 IDF）：
 *   面板：240x296、**4 线 SPI + DC 线**（DC 低=命令、高=数据）、RGB565（**要换字节序**）
 *   接线（与本工作区 5 个 spi_lcd_* 工程完全一致，都在 J2 上）：
 *       SCL=GPIO43(J2-17)  SDA=GPIO44(J2-18)  CS=GPIO45(J2-15)
 *       DC =GPIO46(J2-16)  RST=GPIO47(J2-13)  BL =GPIO48(J2-14)   TE=GPIO49 不用
 *   其余四根（DC/RST/BL）就是普通 GPIO，走 RT-Thread 的 pin 设备。
 *
 * 和 IDF 版的差别：
 *   - 走 RT-Thread 的 `rt_spi_ops`（`spi2` 总线上的 `lcd0` 设备），不是 esp_lcd 组件；
 *   - 我们的 PIO 通路**单次最多 64 字节**（16 字 FIFO），所以整屏数据要切块发，
 *     块间靠 `SPI_MISC.cs_keep_active` 保持 CS 低（否则每 64 字节就抬一次片选，
 *     面板会把后面的数据当成新命令流）；
 *   - 命令/数据分两个 `rt_spi_message`，靠 DC 线切换。
 *
 * msh 命令：
 *   lcd                    初始化（幂等）并打印状态
 *   lcd fill <颜色>        全屏刷一个颜色（r/g/b/black/white/rg/gb/rb，或 0xRRGGBB）
 *   lcd demo               **8 色各刷一遍、共 2 轮**，然后停（默认命令）
 *   lcd clk <hz>          改 SPI 时钟（花屏/闪点时往下降：40M→24M→10M）
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>          /* strtoul */
#include "s31_regs.h"

extern rt_uint64_t s31_systimer_get_ticks(void);      /* drv_systick.c */
extern int s31_spi_set_pins(int mode);                /* drv_spi.c：2 = LCD 档 */

#define LCD_W               240
#define LCD_H               296

/* 引脚（见文件头）*/
#define LCD_PIN_DC          46
#define LCD_PIN_RST         47
#define LCD_PIN_BL          48

#define LCD_SPI_DEV         "lcd0"
#define LCD_SPI_BUS         "spi2"
#define LCD_CLK_HZ_DEFAULT  24000000u    /* IDF 版经验：40MHz 有屏会花，24MHz 稳 */
#define LCD_CHUNK           64u          /* PIO 单次上限 */

/* ---- 面板初始化表（屏厂原始序列）----------------------------------------
 * 来源：`projects\spi_lcd_axs15352\main\axs15352_init_cmds.h`
 *      ← tools\gen_spi_panel_init.py ← 资料\panel_init\...\TianMa2p01+AXS15352_SPI_565_20260716.txt
 * 30 条，逐条：{命令, 数据, 数据长度, 命令之后的延时 ms}。**别手改**。*/
struct lcd_init_cmd {
    rt_uint8_t cmd;
    const rt_uint8_t *data;
    rt_uint8_t len;
    rt_uint8_t delay_ms;
};

#define C(cmd, ...) { cmd, (const rt_uint8_t[]){__VA_ARGS__}, \
                      (rt_uint8_t)(sizeof((const rt_uint8_t[]){__VA_ARGS__})), 0 }
#define C0(cmd)     { cmd, RT_NULL, 0, 0 }
#define CD(cmd, d)  { cmd, RT_NULL, 0, d }

static const struct lcd_init_cmd s_lcd_init[] = {
    C (0xCE, 0x5A, 0xA5),
    C (0xA0, 0x01),
    C (0xA1, 0x00, 0xDD, 0x00, 0x12, 0x32, 0x42, 0xA2, 0x52, 0x61, 0x60, 0x05, 0x40,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x6B, 0xBB, 0x3B,
             0xA5, 0x5A),
    C (0xA2, 0x00, 0x00, 0x00),
    C (0xA4, 0x00, 0x00, 0x22, 0xD3),
    C (0xA5, 0x00),
    C (0xB1, 0x02, 0x00, 0x50),
    C (0xB2, 0x30, 0x30, 0x60, 0x05, 0x03, 0x05, 0x08, 0x42, 0xCF),
    C (0xB3, 0x10, 0x01, 0xFF, 0xF0, 0x0D, 0x10, 0x03, 0x22, 0x00, 0x00, 0x02, 0x22,
             0x4A, 0x02, 0x12, 0x01, 0x42, 0x25, 0x31, 0x5C, 0x65, 0x17),
    C (0xB4, 0x02, 0x50, 0x06, 0x01, 0x30, 0x10, 0x80, 0x6B, 0x03, 0x01, 0x10, 0x10,
             0x00, 0x00, 0x02),
    C (0xB5, 0x00, 0x0A, 0x0A, 0x14, 0x60, 0x80, 0x30, 0x03),
    C (0xB6, 0x28, 0x01, 0xF0, 0x10, 0x28, 0x28, 0x01),
    C (0xB8, 0x00, 0x17, 0xC5, 0x3E, 0x00, 0x03, 0x00, 0x4C, 0x4C, 0x4C, 0x4C, 0x03,
             0x01, 0x00, 0x02, 0x00, 0x03, 0x03, 0x88, 0x60, 0x01, 0x83, 0x01, 0x4C,
             0x4C),
    C (0xB9, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x33, 0x34, 0x00, 0xC3,
             0x2E, 0x02, 0x34, 0x35, 0x00, 0x94, 0x31, 0x02, 0x4C, 0x4C, 0x4C, 0x4C,
             0x4C, 0x4C, 0x4C, 0x4C),
    C (0xBA, 0x28, 0x2B, 0x68),
    C (0xBB, 0x1B, 0x1B, 0x08, 0x0A, 0x0C, 0x0E, 0x00, 0x1F, 0x18, 0x1F, 0x1F, 0x1F),
    C (0xBC, 0x1B, 0x1B, 0x09, 0x0B, 0x0D, 0x0F, 0x01, 0x1F, 0x1A, 0x1F, 0x1F, 0x1F),
    C (0xC0, 0xE0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x50, 0x04, 0x01, 0x80, 0x0C, 0x0C,
             0x28, 0x28, 0x01, 0xF0, 0x0C, 0x0C, 0x0C, 0x0C, 0x00),
    C (0xC3, 0x00, 0x00, 0x00),
    C (0xC4, 0x05, 0x4A, 0x05, 0x0A, 0xE0, 0x2E, 0x00, 0x12, 0x12, 0x22, 0x00, 0x52,
             0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
    C (0xD1, 0x80, 0x14, 0x06),
    C (0xD9, 0x42),
    C (0xE0, 0x00, 0x10, 0x20, 0x30, 0x00, 0x50, 0x6A, 0x8D, 0x9E, 0x00, 0xBF, 0xDD,
             0x04, 0x38, 0x05, 0x5C, 0x76, 0x90, 0xA8, 0x55),
    C (0xE1, 0xC0, 0xC2, 0xD8, 0xF5, 0x55, 0x11, 0x37, 0x5C, 0x95, 0xAA, 0xC4, 0xFE,
             0x0F, 0x1D, 0xAF, 0x30, 0x50, 0x33, 0x70, 0x94, 0xC0, 0xFF, 0xFF),
    C (0xE6, 0xF0, 0x12, 0x22, 0x00, 0xCD, 0x5F, 0xCD, 0x41),
    C (0xF3, 0x05),
    C (0xF4, 0x00, 0x00),
    C (0xCE, 0x00, 0x00),
    CD(0x11, 100),                      /* sleep out，等 100ms */
    C0(0x29),                           /* display on */
};

/* ---- 状态 --------------------------------------------------------------- */
static struct rt_spi_device s_lcd_dev;
static rt_uint8_t s_lcd_ready;
static rt_uint32_t s_lcd_clk_hz = LCD_CLK_HZ_DEFAULT;

static void lcd_dc(int data)
{
    rt_pin_write(LCD_PIN_DC, data ? PIN_HIGH : PIN_LOW);
}

/* 发一条消息（send_buf 可为 NULL 表示只发 len 个 0）；pio 单次 ≤64 字节 */
static void lcd_send(const void *buf, rt_size_t len, int cs_take, int cs_release)
{
    struct rt_spi_message m;

    rt_memset(&m, 0, sizeof(m));
    m.send_buf   = buf;
    m.length     = len;
    m.cs_take    = cs_take ? 1u : 0u;
    m.cs_release = cs_release ? 1u : 0u;
    rt_spi_transfer_message(&s_lcd_dev, &m);
}

/* 面板命令：DC=0 发命令，若带参数则再 DC=1 发数据（CS 全程保持低）*/
static void lcd_send_cmd(rt_uint8_t cmd, const rt_uint8_t *data, rt_size_t len)
{
    lcd_dc(0);
    lcd_send(&cmd, 1, 1, len ? 0 : 1);
    if (len) {
        lcd_dc(1);
        lcd_send(data, len, 0, 1);
    }
}

/* 设置绘图窗口（2A/2B：4 字节，每个坐标 16 位大端）*/
static void lcd_set_window(rt_uint16_t x0, rt_uint16_t y0, rt_uint16_t x1, rt_uint16_t y1)
{
    rt_uint8_t caset[4] = { (rt_uint8_t)(x0 >> 8), (rt_uint8_t)x0,
                            (rt_uint8_t)(x1 >> 8), (rt_uint8_t)x1 };
    rt_uint8_t raset[4] = { (rt_uint8_t)(y0 >> 8), (rt_uint8_t)y0,
                            (rt_uint8_t)(y1 >> 8), (rt_uint8_t)y1 };

    lcd_send_cmd(0x2A, caset, 4);
    lcd_send_cmd(0x2B, raset, 4);
}

static void lcd_apply_clk(void)
{
    struct rt_spi_configuration cfg;

    cfg.data_width = 8;
    cfg.mode       = RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz     = s_lcd_clk_hz;
    rt_spi_configure(&s_lcd_dev, &cfg);
}

/* ---- 对外：初始化 ------------------------------------------------------- */

int s31_lcd_init(void)
{
    rt_uint32_t i;
    rt_uint64_t t0, t1;

    if (s_lcd_ready) {
        return 0;
    }
    if (rt_device_find(LCD_SPI_BUS) == RT_NULL) {
        rt_kprintf("[lcd] 找不到 SPI 总线 %s（drv_spi 没初始化？）\n", LCD_SPI_BUS);
        return -RT_ERROR;
    }

    /* ① 把 SPI 接线切成 LCD 这档：SCL=43 SDA=44 CS=45（硬件 CS0），46/47/48 让给 GPIO */
    s31_spi_set_pins(2);

    /* ② DC / RST / BL 当普通 GPIO */
    rt_pin_mode(LCD_PIN_DC, PIN_MODE_OUTPUT);
    rt_pin_mode(LCD_PIN_RST, PIN_MODE_OUTPUT);
    rt_pin_mode(LCD_PIN_BL, PIN_MODE_OUTPUT);
    rt_pin_write(LCD_PIN_BL, PIN_LOW);              /* 先关背光 */
    lcd_dc(1);

    /* ③ 挂设备到 spi2 总线（CS 由驱动的硬件 CS0 负责，所以 cs_pin 传 PIN_NONE）*/
    if (rt_spi_bus_attach_device_cspin(&s_lcd_dev, LCD_SPI_DEV, LCD_SPI_BUS,
                                       PIN_NONE, RT_NULL) != RT_EOK) {
        rt_kprintf("[lcd] 挂设备 %s 到 %s 失败\n", LCD_SPI_DEV, LCD_SPI_BUS);
        return -RT_ERROR;
    }
    lcd_apply_clk();

    /* ④ 硬复位：RST 低 → 高，等 120ms（厂文件没写，IDF 版用的就是这个量级）*/
    rt_pin_write(LCD_PIN_RST, PIN_LOW);
    rt_thread_mdelay(20);
    rt_pin_write(LCD_PIN_RST, PIN_HIGH);
    rt_thread_mdelay(120);

    /* ⑤ 初始化表（30 条）*/
    t0 = s31_systimer_get_ticks();
    for (i = 0; i < sizeof(s_lcd_init) / sizeof(s_lcd_init[0]); i++) {
        lcd_send_cmd(s_lcd_init[i].cmd, s_lcd_init[i].data, s_lcd_init[i].len);
        if (s_lcd_init[i].delay_ms) {
            rt_thread_mdelay(s_lcd_init[i].delay_ms);
        }
    }
    t1 = s31_systimer_get_ticks();

    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    rt_pin_write(LCD_PIN_BL, PIN_HIGH);             /* 开背光 */

    s_lcd_ready = 1;
    rt_kprintf("[lcd] AXS15352 240x296 ready: SCL=GPIO43 SDA=GPIO44 CS=GPIO45 "
               "DC=GPIO46 RST=GPIO47 BL=GPIO48, SPI %u Hz, init %u 条 %u us\n",
               (unsigned)s_lcd_clk_hz,
               (unsigned)(sizeof(s_lcd_init) / sizeof(s_lcd_init[0])),
               (unsigned)((t1 - t0) / (S31_SYSTIMER_HZ / 1000000u)));
    return 0;
}

/* ---- 对外：全屏填色 ----------------------------------------------------- */

rt_uint32_t s31_lcd_fill(rt_uint16_t rgb565)
{
    rt_uint8_t chunk[LCD_CHUNK];
    rt_uint8_t ramwr = 0x2C;
    rt_uint32_t total = (rt_uint32_t)LCD_W * LCD_H;    /* 71040 像素 */
    rt_uint32_t i, done = 0, chunks;
    rt_uint16_t be = (rt_uint16_t)((rgb565 >> 8) | (rgb565 << 8));   /* 屏要**大端** */
    rt_uint64_t t0, t1;

    if (!s_lcd_ready && s31_lcd_init() != 0) {
        return 0;
    }

    for (i = 0; i < sizeof(chunk); i += 2) {
        chunk[i]     = (rt_uint8_t)(be >> 8);
        chunk[i + 1] = (rt_uint8_t)(be & 0xFF);
    }

    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    /* RAMWR：命令这条**不释放 CS**，紧接着就是像素数据（面板把 2C 之后的所有字节
     * 都当像素；中间抬一次片选就会被当成新命令流）*/
    lcd_dc(0);
    lcd_send(&ramwr, 1, 1, 0);
    lcd_dc(1);

    t0 = s31_systimer_get_ticks();
    chunks = total / (LCD_CHUNK / 2u);             /* 每块 32 像素 */
    while (done < chunks) {
        lcd_send(chunk, LCD_CHUNK, 0, done + 1 == chunks);   /* 最后一块才释放 CS */
        done++;
    }
    t1 = s31_systimer_get_ticks();

    return (rt_uint32_t)((t1 - t0) / (S31_SYSTIMER_HZ / 1000000u));   /* 返回耗时 us */
}

/* ---- msh 命令 ----------------------------------------------------------- */

struct lcd_color { const char *name; rt_uint16_t rgb565; };

static const struct lcd_color s_lcd_colors[] = {
    { "r",     0xF800 }, { "g",     0x07E0 }, { "b",     0x001F },
    { "black", 0x0000 }, { "white", 0xFFFF }, { "rg",    0xFFE0 },
    { "gb",    0x07FF }, { "rb",    0xF81F },
};

static int lcd_color_find(const char *s, rt_uint16_t *out)
{
    rt_uint32_t i;

    for (i = 0; i < sizeof(s_lcd_colors) / sizeof(s_lcd_colors[0]); i++) {
        if (!rt_strcmp(s, s_lcd_colors[i].name)) {
            *out = s_lcd_colors[i].rgb565;
            return 0;
        }
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {         /* 0xRRGGBB */
        rt_uint32_t v = (rt_uint32_t)strtoul(s, RT_NULL, 16);
        rt_uint32_t r = (v >> 16) & 0xFFu, g = (v >> 8) & 0xFFu, b = v & 0xFFu;
        *out = (rt_uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return 0;
    }
    return -1;
}

static void lcd_msh(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "";
    rt_uint32_t us, i, round;

    if (s31_lcd_init() != 0) {
        return;
    }

    if (sub[0] == '\0') {          /* `lcd` 只初始化 + 报状态，不刷屏（要刷得显式 lcd demo）*/
        rt_kprintf("用法: lcd demo（8 色 x 2 轮）| lcd fill <颜色> | lcd clk <hz>\n"
                   "      颜色 = r g b black white rg gb rb（或 0xRRGGBB）\n");
        return;
    }

    if (!rt_strcmp(sub, "clk") && argc > 2) {
        rt_uint32_t hz = (rt_uint32_t)strtoul(argv[2], RT_NULL, 0);
        if (hz < 1000000u || hz > 40000000u) {
            rt_kprintf("时钟范围 1M~40M Hz\n");
            return;
        }
        s_lcd_clk_hz = hz;
        lcd_apply_clk();
        rt_kprintf("[lcd] SPI 时钟改为请求 %u Hz\n", (unsigned)hz);
        return;
    }

    if (!rt_strcmp(sub, "fill") && argc > 2) {
        rt_uint16_t c;
        if (lcd_color_find(argv[2], &c) != 0) {
            rt_kprintf("颜色: r g b black white rg gb rb 或 0xRRGGBB\n");
            return;
        }
        us = s31_lcd_fill(c);
        rt_kprintf("[lcd] fill %s: 全屏 %u 像素 %u us → %u kB/s\n", argv[2],
                   (unsigned)(LCD_W * LCD_H), (unsigned)us,
                   (unsigned)(((rt_uint32_t)LCD_W * LCD_H * 2u) / (us ? us : 1u)));
        return;
    }

    if (rt_strcmp(sub, "demo")) {
        rt_kprintf("用法: lcd | lcd fill <颜色> | lcd demo | lcd clk <hz>\n"
                   "      颜色 = r g b black white rg gb rb（或 0xRRGGBB）\n");
        return;
    }

    /* demo：8 色 × 2 轮，刷完停 */
    rt_kprintf("[lcd] 开始刷屏：8 色 x 2 轮（%dx%d）\n", LCD_W, LCD_H);
    for (round = 1; round <= 2; round++) {
        for (i = 0; i < sizeof(s_lcd_colors) / sizeof(s_lcd_colors[0]); i++) {
            us = s31_lcd_fill(s_lcd_colors[i].rgb565);
            rt_kprintf("  第 %u 轮 %-5s (0x%04X): %u us\n", (unsigned)round,
                       s_lcd_colors[i].name, (unsigned)s_lcd_colors[i].rgb565,
                       (unsigned)us);
        }
    }
    rt_kprintf("[lcd] 刷完 2 轮，停。\n");
}
MSH_CMD_EXPORT_ALIAS(lcd_msh, lcd, SPI LCD (AXS15352 240x296): lcd [demo|fill <color>|clk <hz>]);
