/*===========================================================================
 * main.c -- 应用入口（跑在 RT-Thread 的 main 线程里）
 *
 * msh 不需要我们启动：components/finsh/shell.c:1019 用 INIT_APP_EXPORT
 * 注册了 finsh_system_init()，rt_components_init() 会自动把它拉起来。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>          /* rt_pin_* / PIN_LOW / PIN_MODE_* */
#include <stdlib.h>            /* atoi */
#include "s31_regs.h"
#include "s31_psram.h"         /* PSRAM 初始化 / 访问 / 自检（bsp/s31_psram.c）*/

/* drv_spi.c 提供的自检钩子 */
int  s31_spi_loopback(rt_uint32_t hz);
void s31_spi_miso_pull(int enable);
int  s31_spi_set_pins(int iomux);
void s31_spi_dump_pins(void);
void s31_qspi_sync_config(struct rt_qspi_device *dev);

/* SYSTIMER 计数（16 MHz，与 CPU 频率无关）——drv_systick.c 提供 */
extern rt_uint64_t s31_systimer_get_ticks(void);

/* 等 n 微秒（粗等，用于让焊盘电平稳定）*/
static void wait_us(rt_uint32_t us)
{
    rt_uint64_t t0 = s31_systimer_get_ticks();
    while ((s31_systimer_get_ticks() - t0) < (rt_uint64_t)us * (S31_SYSTIMER_HZ / 1000000u)) {
    }
}

/* 读 32 位周期计数（S31 的 rdcycle 只有 32 位，40MHz 下 ~107 秒回绕） */
static rt_uint32_t rdcycle_lo(void)
{
    rt_uint32_t v;
    __asm__ volatile ("rdcycle %0" : "=r"(v));
    return v;
}

static void s31_info(int argc, char **argv)
{
    extern rt_uint32_t s31_clk_measured_mhz(void);
    (void)argc;
    (void)argv;

    rt_kprintf("---- ESP32-S31 / RT-Thread Nano ----\n");
    rt_kprintf("CPU        : RISC-V RV32IMAFC @%u MHz (rdcycle×systimer 实测)\n",
               (unsigned)s31_clk_measured_mhz());
    rt_kprintf("RAM window : 0x%08x .. 0x%08x\n", (unsigned)S31_RAM_LOW, (unsigned)S31_RAM_HIGH);
    rt_kprintf("rdcycle    : 0x%08x\n", (unsigned)rdcycle_lo());

    /* SYSTIMER 计数器（16MHz → 1us = 16 tick）；读取要走 UPDATE/VALID 握手，
     * 见 bsp/drv_systick.c */
    extern rt_uint64_t s31_systimer_get_ticks(void);
    rt_kprintf("systimer   : %u us since boot\n",
               (unsigned)(s31_systimer_get_ticks() / (S31_SYSTIMER_HZ / 1000000u)));
    rt_kprintf("RT-Thread  : %d.%d.%d\n", RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);
    rt_kprintf("tick       : %u (1ms each)\n", (unsigned)rt_tick_get());
    rt_kprintf("------------------------------------\n");
}
MSH_CMD_EXPORT(s31_info, show S31 board + RT-Thread info);

/* s31_clk：再测一次主频（硬件频率表 + rdcycle×systimer 两个独立口径）*/
static void s31_clk_cmd(int argc, char **argv)
{
    extern rt_uint32_t s31_clk_cpu_mhz_hw(void);
    extern rt_uint32_t s31_clk_status(void);
    extern rt_uint32_t s31_clk_guard_skips(void);
    extern rt_uint32_t s31_clk_guard_addr(void);
    (void)argc; (void)argv;
    rt_kprintf("CPU 硬件频率表: %u MHz   状态寄存器: 0x%08x (bit2 CPLL=%d, bit1 DIV_EFFECT=%d)\n",
               (unsigned)s31_clk_cpu_mhz_hw(), (unsigned)s31_clk_status(),
               (s31_clk_status() >> 2) & 1u, (s31_clk_status() >> 1) & 1u);
    rt_kprintf("提频守卫: 跳过次数 = %u，哨兵@0x%08x（.noinit：复位后仍在，只断电才没）\n",
               (unsigned)s31_clk_guard_skips(), (unsigned)s31_clk_guard_addr());
    rt_kprintf("  复现“上次挂了”：s31_reg w 0x%08x 0x53333143 然后复位 → 下次开机停在 40MHz\n",
               (unsigned)s31_clk_guard_addr());
    rt_kprintf("（更细的 rdcycle×systimer 交叉测量见开机日志里的 [clk] 行）\n");
}
MSH_CMD_EXPORT(s31_clk_cmd, show CPU clock status);

/* s31_cli：把 tick 那套现场摊开（CLIC 的 ip/ie/attr/ctl + 路由 + SYSTIMER 配置）
 * —— 调"tick 不来/只来一次/优先级被阈值屏蔽"时一眼就能看出问题 */
static void s31_cli_cmd(int argc, char **argv)
{
    extern void s31_systick_dump(void);
    (void)argc; (void)argv;
    s31_systick_dump();
}
MSH_CMD_EXPORT(s31_cli_cmd, dump CLIC + SYSTIMER state (tick debugging));

/* reboot：整片复位（走 TIMG0 的 MWDT，**不碰** core0 软复位）
 *
 * 🚨 三条死路（2026-09-23 全踩过，记下来别重犯）：
 *   ① `LP_AONCLKRST_HPCORE0_RESET_CTRL` bit20（core0 软复位，= IDF `esp_restart()` 用那位）
 *      → **core0 被永久按在复位里（PC 恒 0x00000000）**，该位在 AON 域、复位不清，
 *      JTAG 写 0 也救不回来，只能物理 RST。IDF 敢用是因为它前面有二级 bootloader 走完整初始化，
 *      我们是 bootROM 直启，没那一步。
 *   ② RTC 看门狗（`RTC_WDT` CONFIG0 stg0=RESET_SYSTEM）：本 port 没配过 RTC 慢时钟域，
 *      计数器不走 → 等不到复位。
 *   ③ 关掉"超级看门狗"的自动喂狗等它复位：也没等到（自动喂狗像是使能后就锁存了）。
 *   ✅ 现在用 **TIMG0 的 MWDT**：它的时钟是 HP 域的 APB（我们跑 320MHz 时 53.3MHz），
 *      肯定在走；stage0 动作选 RESET_SYSTEM，几百微秒就整片复位。寄存器同 startup.S 里关狗那套：
 *        WDTCONFIG0(+0x48)：bit31 wdt_en、bit[30:29] stg0 动作、bit22 conf_update(WT)
 *        WDTCONFIG1(+0x4c)：stg0 计数、WDTCONFIG2(+0x50)：预分频
 *        WDTWPROTECT(+0x64)：key 0x50D83AA1
 * ⚠️ 特意**不关中断、不 while(1)**：万一看门狗没生效，shell 还活着（能继续敲命令），
 *    而不是像前几版那样把控制台锁死。*/
static void reboot_cmd(int argc, char **argv)
{
    rt_uint32_t timg0 = 0x20580000u;

    (void)argc; (void)argv;

    rt_kprintf("rebooting: 配 TIMG0 的 MWDT（stage0 = 整片复位），等它复位...\n");
    rt_thread_mdelay(20);                              /* 让上面这句先发出去 */

    S31_REG32(timg0 + 0x64u) = 0x50D83AA1u;            /* WDTWPROTECT 解锁 */
    S31_REG32(timg0 + 0x50u) = 1u;                     /* 预分频 = 1（MWDT 时钟 = APB）*/
    S31_REG32(timg0 + 0x4cu) = 2000u;                  /* stg0 计数：2000 拍 */
    S31_REG32(timg0 + 0x48u) = (3u << 29) |            /* STG0 = RESET_SYSTEM */
                               (1u << 31);             /* wdt_en */
    S31_REG32(timg0 + 0x64u) = 0;                      /* 上锁 */

    /* 不 while(1)：正常很快就复位；万一看门狗没生效，shell 还活着能继续敲命令。
     * （实测这条路是通的：复位原因 = `rst:0x7 (HP_SYS_HP_WDT0_RESET)`，
     *   起来后仍 320MHz、守卫正常。）*/
    rt_kprintf("  WDTCONFIG0=0x%08x，复位原因应为 0x07(HP 看门狗0)\n",
               (unsigned)S31_REG32(timg0 + 0x48u));
}
MSH_CMD_EXPORT_ALIAS(reboot_cmd, reboot, restart the chip (TIMG0 MWDT system reset));

/* 演示用：数 100ms 内能跑多少圈（验证 tick 与线程调度是真的在走） */
static void s31_spin(int argc, char **argv)
{
    rt_tick_t start = rt_tick_get();
    rt_uint32_t n = 0;
    while (rt_tick_get() - start < 100) {
        n++;
    }
    rt_kprintf("100 ticks elapsed, looped %u times\n", (unsigned)n);
}
MSH_CMD_EXPORT(s31_spin, count a busy loop for 100 ticks);

/* ---- GPIO 验证 ----------------------------------------------------------
 * 用法： s31_key [次数]        读板载 BOOT 键（GPIO61，低有效；按住是 0）
 *        s31_loop <出> <入>    把两个引脚用杜邦线短接，写出去再读回来（自检）
 * 平时也可以用标准 pin 命令：pin mode 43 1 / pin write 43 1 / pin read 43 */
#define S31_BOOT_KEY_PIN   61

static void s31_key(int argc, char **argv)
{
    int times = (argc > 1) ? atoi(argv[1]) : 10;
    int i;

    rt_pin_mode(S31_BOOT_KEY_PIN, PIN_MODE_INPUT_PULLUP);
    rt_kprintf("BOOT key on GPIO%d (pull-up, active low) — 按住看 0，松开看 1\n",
               S31_BOOT_KEY_PIN);
    for (i = 0; i < times; i++) {
        rt_kprintf("  [%2d] GPIO%d = %d\n", i, S31_BOOT_KEY_PIN,
                   (int)rt_pin_read(S31_BOOT_KEY_PIN));
        rt_thread_mdelay(300);
    }
}
MSH_CMD_EXPORT(s31_key, read BOOT key (GPIO61) a few times);

static void s31_loop(int argc, char **argv)
{
    rt_base_t out_pin, in_pin;
    int v;

    if (argc < 3) {
        rt_kprintf("usage: s31_loop <out_pin> <in_pin>   (把两个脚用杜邦线短接)\n");
        rt_kprintf("  例: s31_loop 43 44   -> J2-17(43) 和 J2-18(44)\n");
        return;
    }
    out_pin = atoi(argv[1]);
    in_pin  = atoi(argv[2]);

    rt_pin_mode(out_pin, PIN_MODE_OUTPUT);
    rt_pin_mode(in_pin, PIN_MODE_INPUT);

    rt_kprintf("loopback GPIO%d -> GPIO%d\n", (int)out_pin, (int)in_pin);
    rt_pin_write(out_pin, PIN_LOW);
    rt_thread_mdelay(5);
    v = (int)rt_pin_read(in_pin);
    rt_kprintf("  write 0 -> read %d %s\n", v, v == 0 ? "OK" : "FAIL");

    rt_pin_write(out_pin, PIN_HIGH);
    rt_thread_mdelay(5);
    v = (int)rt_pin_read(in_pin);
    rt_kprintf("  write 1 -> read %d %s\n", v, v == 1 ? "OK" : "FAIL");
}
MSH_CMD_EXPORT(s31_loop, GPIO loopback test (jumper two pins));

/* ---- I2C 验证：扫描总线 -------------------------------------------------
 * 用法： i2c_scan [bus] [v]   默认扫 i2c0；带 v 打印每个地址的返回值 */
static void i2c_scan(int argc, char **argv)
{
    const char *bus_name = (argc > 1) ? argv[1] : "i2c0";
    int verbose = (argc > 2) && (argv[2][0] == 'v');
    struct rt_i2c_bus_device *bus = rt_i2c_bus_device_find(bus_name);
    int addr, found = 0;

    if (bus == RT_NULL) {
        rt_kprintf("i2c bus '%s' not found\n", bus_name);
        return;
    }

    rt_kprintf("scanning %s ...\n", bus_name);
    for (addr = 1; addr < 128; addr++) {
        struct rt_i2c_msg msg;
        rt_ssize_t r;
        msg.addr  = (rt_uint16_t)addr;
        msg.flags = RT_I2C_WR;          /* 只发地址，看从机应不应答 */
        msg.len   = 0;
        msg.buf   = RT_NULL;

        r = rt_i2c_transfer(bus, &msg, 1);
        if (r == 1) {
            rt_kprintf("  found device at 0x%02x\n", addr);
            found++;
        } else if (verbose) {
            rt_kprintf("  0x%02x -> %d\n", addr, (int)r);
        }
    }
    rt_kprintf("%s: %d device(s)\n", bus_name, found);
}
MSH_CMD_EXPORT(i2c_scan, scan an i2c bus (default i2c0));

/* i2c_dbg [addr]：把一次探测的寄存器现场摊开（默认 0x18）*/
static void i2c_dbg(int argc, char **argv)
{
    extern void s31_i2c_dbg(rt_uint8_t addr);
    rt_uint8_t addr = (argc > 1) ? (rt_uint8_t)strtol(argv[1], RT_NULL, 0) : 0x18;
    s31_i2c_dbg(addr);
}
MSH_CMD_EXPORT(i2c_dbg, dump i2c0 registers around one probe);

/* i2c_wave [addr]：启动一次传输并用 CPU 采样焊盘 → "总线上到底有没有波形" */
static void i2c_wave(int argc, char **argv)
{
    extern void s31_i2c_wave(rt_uint8_t addr);
    rt_uint8_t addr = (argc > 1) ? (rt_uint8_t)strtol(argv[1], RT_NULL, 0) : 0x18;
    s31_i2c_wave(addr);
}
MSH_CMD_EXPORT(i2c_wave, sample SCL/SDA pins during one transfer);

/* i2c_rep [addr] [n]：连打同一个地址 n 次，抓标志位竞争 */
static void i2c_rep(int argc, char **argv)
{
    extern void s31_i2c_rep(rt_uint8_t addr, int times);
    rt_uint8_t addr = (argc > 1) ? (rt_uint8_t)strtol(argv[1], RT_NULL, 0) : 0x18;
    int times = (argc > 2) ? atoi(argv[2]) : 6;
    s31_i2c_rep(addr, times);
}
MSH_CMD_EXPORT(i2c_rep, repeat one probe to expose flag races);

/* ---- SPI 验证：读外部 flash 的 JEDEC ID（0x9F）--------------------------
 * 接线（与工作区 spi_flash_sfud 工程一致）：
 *   SCK=GPIO43(J2-17) MOSI=GPIO44(J2-18) MISO=GPIO45(J2-15) CS=GPIO46(J2-16)
 * W25Q64 应回 EF 40 17。MISO 接法不对时结果也有明确含义（见下面的判读）。*/
/* 一次 0x9F JEDEC 读，返回 3 字节 ID */
static void spi_read_jedec(struct rt_spi_device *dev, rt_uint8_t *id)
{
    rt_uint8_t cmd = 0x9F;
    struct rt_spi_message m1, m2;

    id[0] = id[1] = id[2] = 0;
    rt_memset(&m1, 0, sizeof(m1));
    m1.send_buf   = &cmd;
    m1.length     = 1;
    m1.cs_take    = 1;
    m1.cs_release = 0;
    rt_spi_transfer_message(dev, &m1);

    rt_memset(&m2, 0, sizeof(m2));
    m2.recv_buf   = id;
    m2.length     = 3;
    m2.cs_take    = 0;
    m2.cs_release = 1;
    rt_spi_transfer_message(dev, &m2);
}

/* spi_id [hz]：读 SPI flash 的 JEDEC ID（0x9F）
 * 跑两遍：① MISO 浮空  ② MISO 内部下拉 45k
 *   —— 浮空读数看不出"到底有没有片子"，加上下拉就能判别：
 *      有从机在驱动 MISO 时下拉赢不了（读到 EF 40 17）；
 *      读数被下拉拉成 00 就说明**线上真的没有器件驱动它**（不是我们采错边）。*/
static void spi_id(int argc, char **argv)
{
    struct rt_spi_device *dev = (struct rt_spi_device *)rt_device_find("flash0");
    struct rt_spi_configuration cfg;
    rt_uint8_t id[3], id_pd[3];
    rt_uint32_t hz = (argc > 1) ? (rt_uint32_t)atoi(argv[1]) : 1000000u;

    if (dev == RT_NULL) {
        rt_kprintf("spi device 'flash0' not found\n");
        return;
    }
    cfg.data_width = 8;
    cfg.mode = RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz = hz;
    rt_spi_configure(dev, &cfg);

    spi_read_jedec(dev, id);

    s31_spi_miso_pull(1);
    wait_us(5000);            /* 让悬空脚的电荷被下拉放干净，避免读到"上一状态" */
    spi_read_jedec(dev, id_pd);
    s31_spi_miso_pull(0);

    rt_kprintf("JEDEC ID（浮空读） = %02X %02X %02X  ", id[0], id[1], id[2]);
    if (id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x17) {
        rt_kprintf("= W25Q64 ✓（SPI 通路正常）\n");
    } else {
        rt_kprintf("= 不是 W25Q64\n");
    }

    /* 第二遍：MISO 挂内部下拉。用来区分「没人应答」的两种原因 */
    rt_kprintf("JEDEC ID（MISO 内部下拉 45k）= %02X %02X %02X  →  ",
               id_pd[0], id_pd[1], id_pd[2]);
    if (id_pd[0] == 0xEF) {
        rt_kprintf("有器件在驱动 MISO ✓\n");
    } else if (id_pd[0] == 0x00 && id_pd[1] == 0x00 && id_pd[2] == 0x00) {
        rt_kprintf("下拉把它拉平了 = MISO 悬空，线上没有器件（模块没接/没供电）\n");
    } else if (id_pd[0] == 0xFF) {
        rt_kprintf("下拉拉不动 = 该脚被外部低阻拉高：接了别的东西（例如屏的 CS 上拉），\n"
                   "              但不是会给 0x9F 应答的 flash\n");
    } else {
        rt_kprintf("首字节被读出 0x80：典型是悬空脚/弱上拉在头几个时钟沿没稳住。\n"
                   "              无论哪种，线上都没有器件在应答 0x9F\n");
    }
}
MSH_CMD_EXPORT(spi_id, read SPI flash JEDEC ID (0x9F), arg = hz);

/* spi_loop [hz]：SPI 自环回自检（**不需要外部器件**）
 * 把 MISO 输入信号接到 MOSI 焊盘上，发 64 字节看能不能原样收回，
 * 并用 SYSTIMER 量实际位率。没有这个自检，"读不到 flash"永远分不清
 * 是"驱动错"还是"没接模块"。*/
static void spi_loop(int argc, char **argv)
{
    static const rt_uint32_t freqs[] = {1000000u, 5000000u, 10000000u, 20000000u};
    rt_uint32_t i;

    if (argc > 1) {
        s31_spi_loopback((rt_uint32_t)atoi(argv[1]));
        return;
    }
    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        s31_spi_loopback(freqs[i]);
    }
}
MSH_CMD_EXPORT(spi_loop, SPI self-loopback test (no external parts), arg = hz);

/* spi_pins [matrix|iomux]：切换/查看 SPI 接线方式，并回读焊盘寄存器
 *   matrix = GPIO matrix（SCK=43 MOSI=44 MISO=45 CS=46，手动片选；外接模块默认接法）
 *   iomux  = GPSPI2 专用 IO_MUX 脚（CLK=20 MOSI=21 MISO=22 CS=23 HD=24 WP=25，硬件 CS0）
 *            ⚠️ **四线(QIO)只能用这组**：IDF 的 check_iomux_pins_quad() 就是这么限制的，
 *               GPIO matrix 下数据线在数据相位没法三态。这 6 个脚在板上是 SDIO 的
 *               SD_D0~D3/CLK/CMD（J2 的 26~30 脚，板上没卡座，可自由用）。*/
static void spi_pins(int argc, char **argv)
{
    if (argc > 1) {
        if (!rt_strcmp(argv[1], "matrix")) {
            s31_spi_set_pins(0);
        } else if (!rt_strcmp(argv[1], "iomux")) {
            s31_spi_set_pins(1);
        } else {
            rt_kprintf("用法: spi_pins [matrix|iomux]（不带参数只看当前状态）\n");
            return;
        }
    }
    s31_spi_dump_pins();
}
MSH_CMD_EXPORT(spi_pins, show / switch SPI pin mapping (matrix | iomux));

/* s31_reg r <addr> [n]  —— 读 n 个 32 位字（默认 1）
 * s31_reg w <addr> <val> —— 写一个 32 位字
 * 外设寄存器级排查用（比如"这个焊盘的 OUT_SEL 到底是多少"）。*/
static void s31_reg(int argc, char **argv)
{
    rt_uint32_t addr, val, i, n;

    if (argc < 3 || (argv[1][0] != 'r' && argv[1][0] != 'w')) {
        rt_kprintf("用法: s31_reg r <addr> [n]  |  s31_reg w <addr> <val>\n"
                   "      addr 支持 0x 前缀；例: s31_reg r 0x20583bac\n");
        return;
    }
    addr = (rt_uint32_t)strtoul(argv[2], RT_NULL, 0);

    if (argv[1][0] == 'r') {
        n = (argc > 3) ? (rt_uint32_t)strtoul(argv[3], RT_NULL, 0) : 1u;
        for (i = 0; i < n; i++) {
            rt_kprintf("  [0x%08x] = 0x%08x\n", (unsigned)(addr + 4u * i),
                       (unsigned)S31_REG32(addr + 4u * i));
        }
    } else {
        if (argc < 4) {
            rt_kprintf("缺值: s31_reg w <addr> <val>\n");
            return;
        }
        val = (rt_uint32_t)strtoul(argv[3], RT_NULL, 0);
        S31_REG32(addr) = val;
        rt_kprintf("  [0x%08x] <= 0x%08x  (回读 0x%08x)\n", (unsigned)addr, (unsigned)val,
                   (unsigned)S31_REG32(addr));
    }
}
MSH_CMD_EXPORT(s31_reg, read/write one register: s31_reg r|w <addr> [val|n]);


/* qspi_id [数据线数]：QSPI 读（0x4B，命令/地址 1 线、8 空转、数据 N 线）
 * ⚠️ 数据线数 = 4 时**必须额外接 IO2/IO3（WP/HD）**，只接 4 根线读回来是垃圾。
 * 没有真实从机时，"四个阶段装对了没有"用**时钟数**判定：
 *   1 线一帧 = 命令 8 + 地址 24 + 空转 8 + 数据 8N 位，用 SYSTIMER 量总时长反推位数。*/
static void qspi_id(int argc, char **argv)
{
    struct rt_qspi_device *dev = (struct rt_qspi_device *)rt_device_find("qflash0");
    struct rt_qspi_message qm;
    struct rt_qspi_configuration qcfg;
    rt_uint8_t id[3] = {0};
    int lines = (argc > 1) ? atoi(argv[1]) : 4;
    rt_uint32_t hz = 1000000u;
    rt_uint64_t t0, t1;
    rt_uint32_t us, expect_us;
    rt_err_t rc;

    if (dev == RT_NULL) {
        rt_kprintf("qspi device 'qflash0' not found\n");
        return;
    }

    rt_memset(&qm, 0, sizeof(qm));
    qm.instruction.content    = 0x4B;          /* Quad Output Fast Read */
    qm.instruction.qspi_lines = 1;
    qm.address.content        = 0x000000;
    qm.address.size           = 24;
    qm.address.qspi_lines     = 1;
    qm.dummy_cycles           = 8;
    qm.qspi_data_lines        = (rt_uint8_t)lines;
    qm.parent.recv_buf        = id;
    qm.parent.length          = 3;
    qm.parent.cs_take         = 1;
    qm.parent.cs_release      = 1;

    rt_memset(&qcfg, 0, sizeof(qcfg));
    qcfg.parent.max_hz     = hz;               /* 1MHz：方便用时钟数核对阶段 */
    qcfg.parent.data_width = 8;
    qcfg.parent.mode       = RT_SPI_MODE_0 | RT_SPI_MSB;
    qcfg.medium_size       = 8u * 1024u * 1024u;
    qcfg.qspi_dl_width     = 4;
    rc = rt_qspi_configure(dev, &qcfg);
    s31_qspi_sync_config(dev);          /* ⚠️ 必须：否则上面的配置驱动读不到（见 drv_spi.c）*/
    if (rc != RT_EOK) {
        rt_kprintf("[qspi] configure rc=%d\n", (int)rc);
    }

    t0 = s31_systimer_get_ticks();
    rt_qspi_transfer_message(dev, &qm);
    t1 = s31_systimer_get_ticks();

    us = (rt_uint32_t)((t1 - t0) / (S31_SYSTIMER_HZ / 1000000u));
    /* 一帧的时钟数：命令 8 + 地址 24 + 空转 8（都是 1 线）+ 数据 24 位 / 线数 */
    expect_us = (8u + 24u + 8u) * 1000000u / hz +
                (3u * 8u / (rt_uint32_t)lines) * 1000000u / hz;

    rt_kprintf("QSPI(0x4B, %d 线) ID = %02X %02X %02X\n", lines, id[0], id[1], id[2]);
    rt_kprintf("  一帧耗时 %u us（理论 %u us + 软件开销）→ 命令/地址/空转/数据四阶段装配%s\n",
               (unsigned)us, (unsigned)expect_us,
               (us >= expect_us && us < expect_us + 30u) ? "符合预期 ✓" : "与预期不符 ✗");
    if (lines == 4) {
        rt_kprintf("  ⚠️ 数据相位已按 4 线发出（所以一帧比 1 线短）；但 IO2/IO3(WP/HD)\n"
                   "     没接线时从机回不了数据，读到的字节不可信 —— 要验四线得再接 2 根线\n");
    }
}
MSH_CMD_EXPORT(qspi_id, quad read (0x4B), arg = data lines);

/*===========================================================================
 * PSRAM（bsp/s31_psram.c，board init 里已经初始化好）
 *
 * 从 boot_msc_s31 移植过来的：那份是在真板上把 PSRAM 调通的版本，
 * 两个真凶都不是"看手册能看出来"的 ——
 *   ① PSRAM/MPLL 挂在一个**专用 1.8V LDO** 上，上电默认关着（PMU +0x218/+0x1e8）；
 *   ② **PMA**（RISC-V 自定义 CSR `CSR_PMACFG15`）把外部存储窗口标成只读，
 *      它是 CPU 的 CSR，**寄存器 dump 里看不到**，现象是"一写就 store access fault"。
 *
 * 这一组命令回答两个问题：
 *   · 它到底可不可用  → psram_test（写 pattern 再逐字节读回）+ psram_info
 *   · 它有多快        → psram_speed（拿内部 RAM 做对照）
 *
 * ⚠️ 测速的两个老坑（s31_membench 量化过，别再犯）：
 *   · 用 `volatile` 指针、或 -Og 编出来的循环，量到的是**循环速度**不是内存带宽
 *     （同一块内部 RAM：memset 840 MB/s vs 手写循环 141 MB/s，差 6 倍）
 *     → 这里一律"普通指针 + asm 内存屏障"，且每项取多次的**最小值**
 *   · PSRAM 顺序读受 **cache 行填充**限制（一条 64B 行 ~378ns），
 *     所以"小块反复读"（命中 L1）会明显快过顺序大块读 —— 这是真的，不是量错
 *===========================================================================*/
#define PSRAM_TEST_OFF   0x10000u      /* 测试缓冲放在 64KB 处：让开 init 自己的自检区 */
#define PSRAM_BARRIER()  __asm__ volatile ("" ::: "memory")

static rt_uint32_t us_since(rt_uint64_t t0)
{
    return (rt_uint32_t)((s31_systimer_get_ticks() - t0) / (S31_SYSTIMER_HZ / 1000000u));
}

/* psram_info：状态 + 容量 + 一次 256 字节的原始写/cache 读交叉自检 */
static void psram_info(int argc, char **argv)
{
    rt_uint32_t errs;
    (void)argc; (void)argv;

    rt_kprintf("---- PSRAM ----\n");
    rt_kprintf("  窗口    : 0x%08x .. 0x%08x（映射窗口 %u MB）\n",
               (unsigned)S31_PSRAM_BASE, (unsigned)(S31_PSRAM_BASE + S31_PSRAM_VSIZE),
               (unsigned)(S31_PSRAM_VSIZE >> 20));
    if (!s31_psram_ready()) {
        rt_kprintf("  状态    : ✗ 未初始化成功 —— 看开机日志的 [psram] 行，别去碰这个窗口\n");
        return;
    }
    rt_kprintf("  状态    : ✓ OK\n");
    rt_kprintf("  容量    : %u B = %u MB\n",
               (unsigned)s31_psram_size(), (unsigned)(s31_psram_size() >> 20));

    /* 自检走"原始 MSPI 事务写 + cache 映射窗口读"——正是 App 装载用的那条路 */
    errs = s31_psram_crc_test(S31_PSRAM_BASE, 256);
    rt_kprintf("  自检    : 原始写 + cache 读 256 B -> %s\n",
               (errs == 0) ? "OK（0 字节错）" : "FAIL");
    if (errs != 0 && errs != 0xFFFFFFFFu) {
        rt_kprintf("            错 %u 字节\n", (unsigned)errs);
    }
    rt_kprintf("  下一步  : psram_speed 看带宽，psram_test 做大范围校验\n");
}
MSH_CMD_EXPORT(psram_info, PSRAM status / size / quick self-check);

/* psram_test [KB]：写 pattern + 逐字节读回。
 * 用**普通 store/load**（走 cache）——这就是将来 App 真正用它的方式；
 * 原始事务那条路已经在 init 的交叉自检里验过了。 */
static void psram_test(int argc, char **argv)
{
    rt_uint32_t kb = (argc > 1) ? (rt_uint32_t)atoi(argv[1]) : 256u;
    rt_uint32_t bytes, words, i, errs = 0, err_word = 0xFFFFFFFFu;
    rt_uint32_t *p;
    rt_uint64_t t0;

    if (!s31_psram_ready()) {
        rt_kprintf("PSRAM 没初始化成功 —— 先 psram_info 看状态\n");
        return;
    }
    if (kb == 0u || kb > 4096u) {
        rt_kprintf("KB 取 1..4096\n");
        return;
    }
    bytes = kb * 1024u;
    words = bytes / 4u;
    p = (rt_uint32_t *)(S31_PSRAM_BASE + PSRAM_TEST_OFF);

    t0 = s31_systimer_get_ticks();
    for (i = 0; i < words; i++) {
        p[i] = 0xA5A50000u ^ (i * 2654435761u);      /* 每字都不同：能查出"根本没写进去" */
    }
    PSRAM_BARRIER();
    for (i = 0; i < words; i++) {
        if (p[i] != (0xA5A50000u ^ (i * 2654435761u))) {
            if (errs == 0u) {
                err_word = i;
            }
            errs++;
        }
    }

    rt_kprintf("PSRAM 写读回 %u KB @0x%08x -> %s（错 %u 字，耗时 %u us）\n",
               (unsigned)kb, (unsigned)(S31_PSRAM_BASE + PSRAM_TEST_OFF),
               errs ? "FAIL" : "OK", (unsigned)errs, (unsigned)us_since(t0));
    if (errs) {
        rt_kprintf("  第一个错在字 %u（字节偏移 %u）\n",
                   (unsigned)err_word, (unsigned)(err_word * 4u));
    }
}
MSH_CMD_EXPORT(psram_test, write a pattern into PSRAM and read it back, arg = KB);

/* ---- 测速用的几个循环 --------------------------------------------------
 * 三条规矩，每一条都是踩出来的：
 *   ① **普通指针 + asm 屏障**（不写 `volatile`、不用 -Og）：否则量到的是循环速度
 *      不是内存带宽（s31_membench：同一块内部 RAM，memset 840 vs 手写循环 141 MB/s）
 *   ② **读的结果必须落地**（`g_rd_sink`）：读循环没有副作用，-O2 会整个删掉，
 *      现象是"读 0 us / 0.0 MB/s"（第一次真栽在这，写有内存副作用所以只有读中招）
 *   ③ **4 路展开**：不展开的话"每字 4 周期"的循环自己就成了天花板（≈ 320 MB/s），
 *      比它快的东西（内部 RAM、cache 命中）全都量不出来。展开后每轮 4 个独立 load。*/
static volatile rt_uint32_t g_rd_sink;

static void wr32(rt_uint32_t *p, rt_uint32_t n)
{
    rt_uint32_t i;
    for (i = 0; i + 3u < n; i += 4u) {
        p[i] = i; p[i + 1u] = i + 1u; p[i + 2u] = i + 2u; p[i + 3u] = i + 3u;
    }
    PSRAM_BARRIER();
}

static rt_uint32_t rd32(const rt_uint32_t *p, rt_uint32_t n)
{
    rt_uint32_t i, s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (i = 0; i + 3u < n; i += 4u) {          /* 4 条独立累加链 */
        s0 += p[i]; s1 += p[i + 1u]; s2 += p[i + 2u]; s3 += p[i + 3u];
    }
    PSRAM_BARRIER();
    g_rd_sink = s0 + s1 + s2 + s3;
    return s0 + s1 + s2 + s3;
}

static void wr64(rt_uint64_t *p, rt_uint32_t n)
{
    rt_uint32_t i;
    for (i = 0; i + 1u < n; i += 2u) {
        p[i] = 0x0123456789ABCDEFull | (rt_uint64_t)i;
        p[i + 1u] = 0xFEDCBA9876543210ull | (rt_uint64_t)i;
    }
    PSRAM_BARRIER();
}

static rt_uint64_t rd64(const rt_uint64_t *p, rt_uint32_t n)
{
    rt_uint32_t i;
    rt_uint64_t a = 0, b = 0;
    for (i = 0; i + 1u < n; i += 2u) {
        a += p[i]; b += p[i + 1u];
    }
    PSRAM_BARRIER();
    g_rd_sink = (rt_uint32_t)(a + b);      /* 结果落地，否则读循环被删 */
    return a + b;
}

/* 一行结果：1 字节/µs == 1 MB/s（十进制），所以这个换算不用乘系数 */
static void psram_rep(const char *what, rt_uint32_t bytes, rt_uint32_t us)
{
    rt_uint32_t mb10 = us ? (rt_uint32_t)((rt_uint64_t)bytes * 10u / us) : 0u;
    rt_kprintf("  %-26s %8u us   %5u.%u MB/s\n",
               what, (unsigned)us, (unsigned)(mb10 / 10u), (unsigned)(mb10 % 10u));
}

#define PSRAM_SPEED_ROUNDS  3       /* 每项测几轮，取最小值（首轮常偏慢）*/

static void psram_speed(int argc, char **argv)
{
    rt_uint32_t kb = (argc > 1) ? (rt_uint32_t)atoi(argv[1]) : 256u;
    rt_uint32_t bytes, words;
    rt_uint32_t *ps, *ps2;
    rt_uint32_t *ip = RT_NULL;
    rt_uint32_t best, us, r;
    rt_uint64_t t0;

    if (!s31_psram_ready()) {
        rt_kprintf("PSRAM 没初始化成功 —— 先 psram_info 看状态\n");
        return;
    }
    if (kb == 0u || kb > 4096u) {
        rt_kprintf("KB 取 1..4096\n");
        return;
    }
    bytes = kb * 1024u;
    words = bytes / 4u;
    ps  = (rt_uint32_t *)(S31_PSRAM_BASE + PSRAM_TEST_OFF);
    ps2 = (rt_uint32_t *)(S31_PSRAM_BASE + PSRAM_TEST_OFF + bytes);

    /* 内部 RAM 对照：从堆里拿一块同样大的（拿不到就只测 PSRAM）*/
    ip = (rt_uint32_t *)rt_malloc(bytes);
    if (ip == RT_NULL) {
        rt_kprintf("（内部 RAM 分配 %u KB 失败，只测 PSRAM）\n", (unsigned)kb);
    }

    rt_kprintf("PSRAM 带宽 @%u KB（每项 %u 轮取最小；1 MB/s = 1 B/us）\n",
               (unsigned)kb, (unsigned)PSRAM_SPEED_ROUNDS);

    if (ip) {
        best = 0xFFFFFFFFu;
        for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
            t0 = s31_systimer_get_ticks(); wr32(ip, words); us = us_since(t0);
            if (us < best) { best = us; }
        }
        psram_rep("内部RAM 写 u32(对照)", bytes, best);

        best = 0xFFFFFFFFu;
        for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
            t0 = s31_systimer_get_ticks(); (void)rd32(ip, words); us = us_since(t0);
            if (us < best) { best = us; }
        }
        psram_rep("内部RAM 读 u32(对照)", bytes, best);
    }

    best = 0xFFFFFFFFu;
    for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
        t0 = s31_systimer_get_ticks(); wr32(ps, words); us = us_since(t0);
        if (us < best) { best = us; }
    }
    psram_rep("PSRAM 写 u32", bytes, best);

    best = 0xFFFFFFFFu;
    for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
        t0 = s31_systimer_get_ticks(); (void)rd32(ps, words); us = us_since(t0);
        if (us < best) { best = us; }
    }
    psram_rep("PSRAM 读 u32", bytes, best);

    best = 0xFFFFFFFFu;
    for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
        t0 = s31_systimer_get_ticks(); wr64((rt_uint64_t *)ps, words / 2u); us = us_since(t0);
        if (us < best) { best = us; }
    }
    psram_rep("PSRAM 写 u64", bytes, best);

    best = 0xFFFFFFFFu;
    for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
        t0 = s31_systimer_get_ticks(); (void)rd64((const rt_uint64_t *)ps, words / 2u); us = us_since(t0);
        if (us < best) { best = us; }
    }
    psram_rep("PSRAM 读 u64", bytes, best);

    best = 0xFFFFFFFFu;
    for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
        t0 = s31_systimer_get_ticks(); rt_memset(ps, 0x5A, bytes); us = us_since(t0);
        if (us < best) { best = us; }
    }
    psram_rep("PSRAM memset", bytes, best);

    best = 0xFFFFFFFFu;
    for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
        t0 = s31_systimer_get_ticks(); rt_memcpy(ps2, ps, bytes); us = us_since(t0);
        if (us < best) { best = us; }
    }
    psram_rep("PSRAM->PSRAM memcpy", bytes, best);

    if (ip) {
        best = 0xFFFFFFFFu;
        for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
            t0 = s31_systimer_get_ticks(); rt_memcpy(ps, ip, bytes); us = us_since(t0);
            if (us < best) { best = us; }
        }
        psram_rep("内部RAM->PSRAM memcpy", bytes, best);
        rt_free(ip);
    }

    /* cache 命中档：反复读同样 16KB —— 量的是 L1 带宽，不是 PSRAM 带宽。
     * 放在最后是因为它跟上面几个不是一个量纲，别混着看。*/
    if (bytes >= 16384u) {
        rt_uint32_t win_words = 16384u / 4u, reps = (bytes / 16384u) * 4u;
        best = 0xFFFFFFFFu;
        for (r = 0; r < PSRAM_SPEED_ROUNDS; r++) {
            t0 = s31_systimer_get_ticks();
            for (us = 0; us < reps; us++) { (void)rd32(ps, win_words); }
            {
                rt_uint32_t us2 = us_since(t0);
                if (us2 < best) { best = us2; }
            }
        }
        psram_rep("PSRAM 16KB 反复读(命中L1)", 16384u * reps, best);
    }

    rt_kprintf("  提示: 顺序读受 cache 行填充限制（64B/行 ≈378ns）；\n");
    rt_kprintf("        最后那行「16KB 反复读」量的是 **L1 命中带宽**（≈510 MB/s），\n");
    rt_kprintf("        跟前面几行不是一个量纲 —— 差 4 倍是正常的，别当成 PSRAM 变快了\n");
}
MSH_CMD_EXPORT(psram_speed, PSRAM read/write bandwidth (internal RAM as reference), arg = KB);

int main(void)
{
    extern void s31_clk_guard_clear(void);
    extern rt_uint32_t s31_clk_guard_skips(void);

    rt_kprintf("\n[RT-Thread Nano / ESP32-S31] msh 已就绪：敲 help 看命令，"
               "s31_info 看板子信息，ps 看线程\n");

    /* 提频守卫确认：真的跑到这里（线程都起来了）才撤哨兵 ——
     * 万一 320MHz 后面才挂，下次上电会自动退回 40MHz（见 drv_clk.c 的守卫）。*/
    rt_thread_mdelay(200);
    s31_clk_guard_clear();
    if (s31_clk_guard_skips()) {
        rt_kprintf("[clk] 守卫曾跳过提频 %u 次（可用 s31_clk_cmd 查看）\n",
                   (unsigned)s31_clk_guard_skips());
    }
    return 0;
}
