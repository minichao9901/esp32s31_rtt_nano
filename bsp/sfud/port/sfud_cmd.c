/*===========================================================================
 * sfud_cmd.c -- sfud 的 msh 命令（`sf ...`）
 *
 * 命令一览（敲 `sf` 看帮助）：
 *   sf probe [spi_dev] [hz]        识别芯片（默认 spi_dev=flash0、hz=20MHz）
 *   sf info                        芯片参数 + SFDP + 擦除器 + 当前绑定
 *   sf clk [hz]                    换 SPI 时钟（不加参数只报告；默认 20MHz）
 *   sf read <addr> [len]           读 + 十六进制 dump（默认 64 字节）
 *   sf write <addr> <hex>          先擦后写（0x02 页编程），可写跨页
 *   sf erase <addr> <len>          擦（按擦除粒度向上对齐，4KB 扇区）
 *   sf status [vol bit]            读状态寄存器 / 写（vol=1 易失、0 非易失）
 *   sf test <addr> [len]           **完整自检**：擦 → 验全 FF → 写花样 → 逐字节读回校验
 *   sf bench <addr> <len>          速度基准：擦 / 写 / 读（含校验）+ 分块长度对照
 *
 * ⚠️ 三条安全约定（都写进代码里了）：
 *   ① 所有破坏性操作（erase / write / test / bench）**必须先带地址**，
 *      不存在"不带参数就整片擦"的路径 —— 上游那个 `sf bench` 是整片擦，
 *      这块板子的外接 flash 未必是空的，不能默认整片抹掉。
 *   ② `sf test` / `sf bench` 只动你给的那一段。
 *   ③ 地址/长度做边界检查（都在 chip.capacity 内），越界直接拒绝。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>

#include "sfud.h"
#include "sfud_port.h"
#include "s31_regs.h"           /* S31_SYSTIMER_HZ */

extern rt_uint64_t s31_systimer_get_ticks(void);        /* drv_systick.c */

#define SF_DEFAULT_READ_LEN     64u
#define SF_TEST_LEN_DEFAULT     4096u
#define SF_BENCH_LEN_DEFAULT    4096u
#define SF_HEXDUMP_WIDTH        16

/* 一次读多大（`sf read` 超过就截断，防止一条命令把堆吃光）*/
#define SF_READ_MAX             4096u
/* 校验缓冲：内部 RAM 就 460KB，取大小两头兼顾 */
#define SF_BUF_MAX              (64u * 1024u)

/*---------------------------------------------------------------------------
 * 小工具
 *-------------------------------------------------------------------------*/
static rt_uint32_t sf_elapsed_us(rt_uint64_t t0)
{
    rt_uint32_t us = (rt_uint32_t)((s31_systimer_get_ticks() - t0) /
                                   (S31_SYSTIMER_HZ / 1000000u));
    return us ? us : 1u;              /* 别让后面除零 */
}

/* 一行速度。
 * 🚨 别用"MB/s 保留 1 位小数"：4KB 擦除只有 **0.085 MB/s**，整数截断后显示成
 *    `0.0 MB/s` —— 看着像"根本没跑"，实际是显示的问题（用户就踩过这个）。
 *    所以小于 1 MB/s 一律换成 kB/s 显示。1 字节/us == 1 MB/s（十进制）。*/
static void sf_rep(const char *what, rt_uint32_t bytes, rt_uint32_t us)
{
    rt_uint64_t kbs = (rt_uint64_t)bytes * 1000u / (us ? us : 1u);   /* kB/s */

    if (kbs >= 1000u) {
        rt_kprintf("  %-28s %8u us   %u.%02u MB/s\n", what, (unsigned)us,
                   (unsigned)(kbs / 1000u), (unsigned)((kbs % 1000u) / 10u));
    } else {
        rt_kprintf("  %-28s %8u us   %u kB/s\n", what, (unsigned)us, (unsigned)kbs);
    }
}

static sfud_err sf_read_retry(sfud_flash *flash, rt_uint32_t addr, rt_uint32_t len, rt_uint8_t *buf)
{
    sfud_err e = sfud_read(flash, addr, len, buf);
    if (e != SFUD_SUCCESS) {
        e = sfud_read(flash, addr, len, buf);       /* 读失败没有内部重试，这里补一次 */
    }
    return e;
}

/* 取当前 flash（没 probe 过就提示并返回 NULL）*/
static sfud_flash *sf_cur(void)
{
    sfud_flash *flash = sfud_get_device(0);

    if (flash == RT_NULL || !flash->init_ok || s31_sfud_dev_num() == 0u) {
        rt_kprintf("还没识别到 flash —— 先跑 `sf probe`（默认用 SPI 设备 flash0）\n");
        return RT_NULL;
    }
    return flash;
}

/* 写粒度（一次"页编程"最多几个字节）：
 *   ⚠️ SFUD 的 `sfud_flash_chip` 里**没有** write_gran 这个字段（只有 erase_gran）——
 *      写粒度是 hardware_init 内部算出来喂给 page256_or_1_byte_write() 的局部量，
 *      SFDP 有就用 SFDP 的，没有就退回 SFUD_WRITE_MAX_PAGE_SIZE（256）。
 *      所以这里照同一套规则显示，"模拟真实写文件的姿势"那一步也用它。*/
static rt_uint32_t sf_write_gran(const sfud_flash *flash)
{
#ifdef SFUD_USING_SFDP
    if (flash->sfdp.available && flash->sfdp.write_gran != 0u) {
        return flash->sfdp.write_gran;
    }
#endif
    return SFUD_WRITE_MAX_PAGE_SIZE;
}

/* 型号名：🚨 **SFDP 走通时上游会把 `chip.name` 置成 NULL**（sfud.c:284 ——
 * 型号名只有"查内置型号表"那条路才会填）。所以外面直接用 `%s` 会打出 "(null)"。
 * 这里给它一个能看的替代串（名字 + JEDEC ID 三个字节）。*/
static const char *sf_chip_name(const sfud_flash *flash)
{
    return (flash->chip.name != RT_NULL) ? flash->chip.name : "(SFDP 模式不给型号名)";
}

/* 每次测速/自检都在标题里报一遍当前 SPI 时钟 —— 不然"结果慢了 10 倍"会被当成
 * 驱动问题，其实往往是上一条命令（`sf bb` / `spi_id` 都会把总线配到 1 MHz）
 * 把时钟留在低速档上了。`sf bench` / `sf test` 不负责改时钟，只如实显示。*/
static void sf_show_clock(void)
{
    rt_uint32_t hz = s31_sfud_cur_hz();

    rt_kprintf("  SPI 时钟 %u Hz%s\n", (unsigned)hz,
               (hz >= 1000000u) ? "" : "  ⚠️ 低速档：上一条命令（sf bb/spi_id）留下的，`sf probe` 可回到 20 MHz");
}

/* 制造商名：SFUD 的 mf_table 是个 static 数组（sfud.c:42），外面看不到，
 * 所以这里照 def.h 的 SFUD_MF_TABLE 列一遍常见的（对不上就报 ID 本身）。*/
static const char *sf_mf_name(rt_uint8_t mf_id)
{
    switch (mf_id) {
    case 0x01: return "Cypress";
    case 0x04: return "Fujitsu";
    case 0x1C: return "EON";
    case 0x1F: return "Atmel";
    case 0x20: return "Micron";
    case 0x37: return "AMIC";
    case 0x52: return "NOR_MEM";
    case 0x62: return "SANYO";
    case 0x89: return "Intel";
    case 0x8C: return "ESMT";
    case 0xA1: return "Fudan";
    case 0xAD: return "Hyundai";
    case 0xBF: return "SST";
    case 0xC2: return "Macronix";
    case 0xC8: return "GigaDevice";
    case 0xD5: return "ISSI";
    case 0xEF: return "Winbond";
    default:   return "未知厂商";
    }
}

static const char *sf_err_str(sfud_err e)
{
    switch (e) {
    case SFUD_SUCCESS:              return "OK";
    case SFUD_ERR_NOT_FOUND:        return "NOT_FOUND(0x01)";
    case SFUD_ERR_WRITE:            return "WRITE(0x02)";
    case SFUD_ERR_READ:             return "READ(0x03)";
    case SFUD_ERR_TIMEOUT:          return "TIMEOUT(0x04)";
    case SFUD_ERR_ADDR_OUT_OF_BOUND:return "ADDR_OUT_OF_BOUND(0x05)";
    default:                        return "?";
    }
}

/*---------------------------------------------------------------------------
 * sf probe [spi_dev] [hz]
 *-------------------------------------------------------------------------*/
static void sf_cmd_probe(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "flash0";
    rt_uint32_t hz  = (argc > 2) ? (rt_uint32_t)strtoul(argv[2], RT_NULL, 0) : 0u;
    sfud_flash *flash;
    rt_err_t rc;

    rc = s31_sfud_probe("spi_flash0", dev, hz);
    if (rc != RT_EOK) {
        return;                     /* 具体原因 s31_sfud_probe 里已经打了 */
    }

    flash = sfud_get_device(0);
    if (flash == RT_NULL) {
        rt_kprintf("[sfud] 内部设备表是空的？这不该发生\n");
        return;
    }
    rt_kprintf("  %s %s，容量 %u KB（%u 字节）\n",
               sf_mf_name(flash->chip.mf_id), sf_chip_name(flash),
               (unsigned)(flash->chip.capacity / 1024u), (unsigned)flash->chip.capacity);
    rt_kprintf("  JEDEC ID %02X %02X %02X，擦除粒度 %u B / 页 %u B，SFDP %s\n",
               flash->chip.mf_id, flash->chip.type_id, flash->chip.capacity_id,
               (unsigned)flash->chip.erase_gran,
               (unsigned)sf_write_gran(flash),
               flash->sfdp.available ? "可用" : "不可用（走内置型号表）");
}

/*---------------------------------------------------------------------------
 * SPI2 寄存器快照（排"事务跑着跑着就不通了"用）
 *
 * 为什么需要它：现象是"第 1 次事务好、第 2 次开始永远读回 0"，而
 * `spi_loop`（环回）连跑三次都好 —— 因为**环回是把 MISO 内部接到自己的
 * MOSI，不需要片选也能过**，所以环回根本测不到 CS。于是嫌疑落在
 * "CS 没动作"或"某个配置寄存器被事务改掉了"上，这一 dump 就能看出来。
 *-------------------------------------------------------------------------*/
static void sf_dump_spi2(const char *tag)
{
    const rt_uint32_t b = 0x2038F000u;      /* GPSPI2 基址 */
    const rt_uint32_t G = 0x20583000u;      /* GPIO 基址   */

#define SF_RD(a)  (unsigned)S31_REG32(a)
    rt_kprintf("  --- SPI2 快照 [%s] ---\n", tag);
    rt_kprintf("      CMD=%08X CTRL=%08X CLOCK=%08X USER=%08X\n",
               SF_RD(b + 0x00u), SF_RD(b + 0x08u), SF_RD(b + 0x0Cu), SF_RD(b + 0x10u));
    rt_kprintf("      USER1=%08X USER2=%08X MS_DLEN=%08X MISC=%08X\n",
               SF_RD(b + 0x14u), SF_RD(b + 0x18u), SF_RD(b + 0x1Cu), SF_RD(b + 0x20u));
    rt_kprintf("      DIN_MODE=%08X DIN_NUM=%08X DOUT_MODE=%08X\n",
               SF_RD(b + 0x24u), SF_RD(b + 0x28u), SF_RD(b + 0x2Cu));
    rt_kprintf("      DMA_CONF=%08X DMA_INT_RAW=%08X\n",
               SF_RD(b + 0x30u), SF_RD(b + 0x3Cu));
    /* CS(=GPIO46) / SCK(43) / MOSI(44) / MISO(45) 的焊盘寄存器 */
    rt_kprintf("      pad43 outsel=%u pad44 outsel=%u pad46(CS) outsel=%u insel54=%u\n",
               SF_RD(G + 0xAF4u + 4u * 43u), SF_RD(G + 0xAF4u + 4u * 44u),
               SF_RD(G + 0xAF4u + 4u * 46u), SF_RD(G + 0x2F4u + 4u * 54u));
    /* IO_MUX（基址 0x20582000，偏移查 s31_iomux_off[]：43→0xac 44→0xb0 45→0xb4 46→0xb8）：
     * bit12 起 = MCU_SEL，bit9 = FUN_IE（输入缓冲）、bit8 = PU、bit7 = PD。
     * 🚨 MISO(45) 的 **FUN_IE 一旦是 0**，从机就算在应答也听不见 —— 读出来全是 0，
     *    现象和"模块没接"一模一样。这份 dump 就是为了抓这个。*/
    rt_kprintf("      iomux43=%08X iomux44=%08X iomux45(MISO)=%08X iomux46(CS)=%08X\n",
               SF_RD(0x20582000u + 0xACu), SF_RD(0x20582000u + 0xB0u),
               SF_RD(0x20582000u + 0xB4u), SF_RD(0x20582000u + 0xB8u));
    /* GPIO 第二组（32~63）：ENABLE1 在 +0x40、OUT1 在 +0x10、IN1 在 +0x68 */
    rt_kprintf("      gpio_enable1=%08X gpio_out1=%08X gpio_in1=%08X\n",
               SF_RD(G + 0x40u), SF_RD(G + 0x10u), SF_RD(G + 0x68u));
#undef SF_RD
}

/*---------------------------------------------------------------------------
 * sf dbg —— 裸事务诊断（**不经过 SFUD**）
 *
 * 为什么需要它：`sf probe` 失败时，SFUD 只会说"没找到/不支持"，看不出
 * **线上到底回了什么**。这里直接把三条最要紧的命令各发一遍、把原始字节打出来：
 *   0x9F JEDEC ID      → EF 40 17 = W25Q64
 *   0x90 制造商/器件 ID → EF 40 17（和 0x9F 同值，但走的是另一条命令路径）
 *   0x5A SFDP 头 8 字节 → 必须 "SFDP" 开头，否则 SFDP 走不通（会自动退回型号表）
 * 另外顺着 MOSI 环回（把 MISO 输入源接到 MOSI 焊盘）自证"时钟/FIFO 通路是活的"
 * —— 和 `spi_loop` 一个思路，但用的是同一条 0x9F 命令路径。
 *-------------------------------------------------------------------------*/
static void sf_dbg(int argc, char **argv)
{
    rt_uint8_t cmd[5], rx[8];
    rt_uint32_t hz = (argc > 1) ? (rt_uint32_t)strtoul(argv[1], RT_NULL, 0) : 0u;
    rt_uint32_t i;
    rt_err_t rc;

    /* 第一步只**绑定**（配 SPI、建锁），不跑 SFUD 的识别 —— 这样下面那次裸读
     * 才是"SFUD 动手之前"的总线状态。理由见 s31_sfud_bind() 的说明。*/
    if (s31_sfud_bind("flash0", hz ? hz : 1000000u) != RT_EOK) {
        rt_kprintf("[sf dbg] 连 SPI 设备都没绑上，先查 `spi_pins` / drv_spi 的初始化\n");
        return;
    }

    rt_kprintf("---- sf dbg: SPI 设备 \"%s\" @ %u Hz ----\n",
               s31_sfud_bind_dev(), (unsigned)s31_sfud_cur_hz());

    /* ① SFUD 动手**之前**先裸读一次 0x9F：把"总线本身不通"和"SFUD 跑完才不通"
     *    分开 —— 两次结果一比就知道该查接线还是查驱动。
     *    （2026-09-25 抓 CS 那个 bug 时，正是这一步把范围锁到 drv_spi.c 的。）*/
    cmd[0] = 0x9Fu;
    rt_memset(rx, 0, sizeof(rx));
    rc = s31_sfud_raw_xfer(cmd, 1u, rx, 3u);
    rt_kprintf("  裸读 0x9F（SFUD 之前）: %s\n", (rc != RT_EOK) ? "事务失败"
               : (rx[0] == 0xEFu && rx[1] == 0x40u) ? "EF 40 17（通）"
               : (rx[0] == 0x00u) ? "00 00 00（不通）" : "认不出来");

    /* ② 再跑一遍 SFUD 的识别，看它之后还通不通 */
    {
        rt_err_t prc = s31_sfud_probe("spi_flash0", "flash0", hz ? hz : 1000000u);
        rt_kprintf("  SFUD 识别是否成功     : %s\n", (prc == RT_EOK) ? "是" : "否");
    }
    rt_memset(rx, 0, sizeof(rx));
    rc = s31_sfud_raw_xfer(cmd, 1u, rx, 3u);
    rt_kprintf("  裸读 0x9F（SFUD 之后）: %s\n", (rc != RT_EOK) ? "事务失败"
               : (rx[0] == 0xEFu && rx[1] == 0x40u) ? "EF 40 17（通）"
               : (rx[0] == 0x00u) ? "00 00 00（不通）" : "认不出来");
    sf_dump_spi2("两次裸读之后");

    /* ① 0x9F JEDEC ID —— 连打 4 次，中间各隔 2ms。
     * 为什么要连打：探针常遇到"**第一次读全 0，隔一会儿再读就正常**"，
     * 那是模块上电后还没稳（或 MISO 是悬空脚、靠寄生电容保持上次电平）。
     * 单看一次会把它误判成"没接模块"。*/
    cmd[0] = 0x9Fu;
    rt_kprintf("  0x9F JEDEC ID  : ");
    for (i = 0; i < 4u; i++) {
        rt_memset(rx, 0, sizeof(rx));
        rc = s31_sfud_raw_xfer(cmd, 1u, rx, 3u);
        if (rc != RT_EOK) {
            rt_kprintf("[#%u 事务失败 rc=%d] ", (unsigned)i, (int)rc);
        } else {
            rt_kprintf("[#%u %02X %02X %02X] ", (unsigned)i, rx[0], rx[1], rx[2]);
        }
        rt_thread_mdelay(2);
    }
    rt_kprintf("\n");
    {
        /* 判读按"最后一次"来：模块刚上电时前几次可能是 0 */
        rt_uint8_t a = rx[0], b = rx[1], c = rx[2];
        rt_kprintf("                   → %s\n",
                   (a == 0xEFu && b == 0x40u && c == 0x17u) ? "= W25Q64 ✓" :
                   (a == 0x00u && b == 0x00u && c == 0x00u) ? "= 全 0：MISO 被拉平（模块没接/没供电）" :
                   (a == 0xFFu && b == 0xFFu) ? "= 全 1：该脚被外部拉高（接了别的东西）" :
                   (a == 0x80u) ? "= 首字节 0x80：头几个时钟沿没稳住（悬空脚/弱上拉）" :
                   "= 认不出来");
    }

    /* ② 0x90 制造商/器件 ID（地址 0 + 3 空转字节）*/
    cmd[0] = 0x90u; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0;
    rt_memset(rx, 0, sizeof(rx));
    rc = s31_sfud_raw_xfer(cmd, 5u, rx, 2u);
    if (rc == RT_EOK) {
        rt_kprintf("  0x90 器件 ID   : %02X %02X\n", rx[0], rx[1]);
    } else {
        rt_kprintf("  0x90 器件 ID   : 事务失败 rc=%d\n", (int)rc);
    }

    /* ③ 0x5A SFDP 头（地址 0 + 1 空转字节，收 8 字节）*/
    cmd[0] = 0x5Au; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0;
    rt_memset(rx, 0, sizeof(rx));
    rc = s31_sfud_raw_xfer(cmd, 5u, rx, 8u);
    if (rc != RT_EOK) {
        rt_kprintf("  0x5A SFDP 头   : 事务失败 rc=%d\n", (int)rc);
    } else {
        rt_kprintf("  0x5A SFDP 头   : %02X %02X %02X %02X | %02X %02X %02X %02X  ",
                   rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
        if (rx[0] == 'S' && rx[1] == 'F' && rx[2] == 'D' && rx[3] == 'P') {
            rt_kprintf("= \"SFDP\" ✓ rev %u.%u，%u 个参数头\n",
                       (unsigned)rx[5], (unsigned)rx[4], (unsigned)rx[6]);
        } else if (rx[0] == 0x00u && rx[1] == 0x00u) {
            rt_kprintf("= 全 0（这条命令没被应答）\n");
        } else if (rx[0] == 0xFFu) {
            rt_kprintf("= 全 1（MISO 被拉高）\n");
        } else {
            rt_kprintf("= 不是 SFDP 魔数（芯片可能不支持 SFDP，或这条命令的时序不对）\n");
        }
    }

    /* ④ 状态寄存器（0x05）：这条通了说明"命令+读"的基本路径没问题 */
    cmd[0] = 0x05u;
    rt_memset(rx, 0, sizeof(rx));
    rc = s31_sfud_raw_xfer(cmd, 1u, rx, 1u);
    if (rc != RT_EOK) {
        rt_kprintf("  0x05 状态寄存器: 事务失败 rc=%d\n", (int)rc);
    } else {
        rt_kprintf("  0x05 状态寄存器: 0x%02X（BUSY=%u WEL=%u BP=%u%u%u）\n", rx[0],
                   (unsigned)((rx[0] >> 0) & 1u), (unsigned)((rx[0] >> 1) & 1u),
                   (unsigned)((rx[0] >> 4) & 1u), (unsigned)((rx[0] >> 3) & 1u),
                   (unsigned)((rx[0] >> 2) & 1u));
    }

    /* ⑤ 读 0x000000 的前 16 字节：能读出来就说明 0x03 那条路也通 */
    cmd[0] = 0x03u; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0;
    rc = s31_sfud_raw_xfer(cmd, 4u, rx, sizeof(rx));
    if (rc == RT_EOK) {
        rt_kprintf("  0x03 读 0x000000: ");
        for (i = 0; i < sizeof(rx); i++) {
            rt_kprintf("%02X ", rx[i]);
        }
        rt_kprintf("\n");
    }

    if (s31_sfud_dev_num() != 0u) {
        rt_kprintf("  （SFUD 已识别：%s；`sf info` 看完整参数）\n", sf_chip_name(sfud_get_device(0)));
    } else {
        rt_kprintf("  （SFUD 尚未识别：上面的字节就是判断依据）\n");
    }
}

/*---------------------------------------------------------------------------
 * sf bb —— **软件位翻转（bit-bang）**读 JEDEC ID，连读 5 次
 *
 * 为什么要它：到这一步为止，所有"读回来是全 0"的解释都只在 SPI 外设这一侧，
 * 而外设的寄存器对账又是完全自洽的。那就把外设整个摘掉，用普通 GPIO 手动
 * 打 SPI mode0 的时序去读同一块 flash：
 *   · 位翻转 5 次**每次都读到 EF 40 17** ⇒ flash、接线、供电全是好的，
 *     问题 100% 在 drv_spi.c 的 PSRAM…啊不，在它的外设通路上；
 *   · 位翻转也只有第一次对 ⇒ 那问题在模块/接线/供电（该查线了）。
 * 这条命令不改 flash 内容，跑完会自动把 SPI 路由装回去。
 *-------------------------------------------------------------------------*/
#define BB_SCK      43
#define BB_MOSI     44
#define BB_MISO     45
#define BB_CS       46
#define BB_SEL_GPIO 256u                 /* OUT_SEL 写这个值 = 输出源取 GPIO_OUT 寄存器 */

/* 把焊盘从 SPI 外设手里摘下来，改成"普通 GPIO、由 GPIO_OUT 寄存器驱动" */
static void bb_pin_take(rt_base_t pin)
{
    const rt_uint32_t G = 0x20583000u;
    S31_REG32(G + 0xAF4u + 4u * (rt_uint32_t)pin) = BB_SEL_GPIO;
}

static void bb_delay(void)
{
    /* 只为了让电平稳定；flash 没有最低时钟频率，慢一点无所谓 */
    __asm__ volatile ("nop; nop; nop; nop; nop; nop; nop; nop;");
}

/* mode0：上升沿采样，MSB 在前 */
static rt_uint8_t bb_xfer_byte(rt_uint8_t out)
{
    rt_uint8_t in = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        rt_pin_write(BB_MOSI, (out >> i) & 1u);
        bb_delay();
        rt_pin_write(BB_SCK, 1);
        bb_delay();
        in = (rt_uint8_t)((in << 1) | (rt_pin_read(BB_MISO) ? 1u : 0u));
        rt_pin_write(BB_SCK, 0);
        bb_delay();
    }
    return in;
}

/* 单独发一条"只有命令码"的短命令（CS 拉低 → 发 → 抬起）*/
static void bb_send_cmd1(rt_uint8_t c)
{
    rt_pin_write(BB_CS, 0);
    bb_delay();
    (void)bb_xfer_byte(c);
    rt_pin_write(BB_CS, 1);
    bb_delay();
}

static void bb_read_jedec(rt_uint8_t *id)
{
    rt_pin_write(BB_CS, 0);
    bb_delay();
    (void)bb_xfer_byte(0x9Fu);
    id[0] = bb_xfer_byte(0xFFu);
    id[1] = bb_xfer_byte(0xFFu);
    id[2] = bb_xfer_byte(0xFFu);
    rt_pin_write(BB_CS, 1);
    bb_delay();
}

static void sf_bb(int argc, char **argv)
{
    extern int s31_spi_set_pins(int mode);
    rt_uint8_t id[3];
    int k, pass = 0;
    /* 进来之前总线是什么状态（时钟/设备），跑完还回去 ——
     * 这条命令为了稳会把总线绑到 1 MHz，不还的话后面 `sf bench` 就慢 20 倍，
     * 而现象只会是"速度变慢了"，很难联想到是上一条命令留下的（踩过）。*/
    rt_uint32_t hz_before = s31_sfud_cur_hz();
    char dev_before[RT_NAME_MAX] = {0};
    (void)argc; (void)argv;

    if (s31_sfud_bind_dev() != RT_NULL) {
        rt_strncpy(dev_before, s31_sfud_bind_dev(), sizeof(dev_before) - 1u);
    }

    rt_kprintf("---- sf bb: 软件位翻转读 0x9F（绕开 SPI 外设）----\n");
    rt_kprintf("  接线假定: SCK=%d MOSI=%d MISO=%d CS=%d（J2-17/18/15/16）\n",
               BB_SCK, BB_MOSI, BB_MISO, BB_CS);

    /* 1) 摘外设路由 + 配成普通 GPIO */
    bb_pin_take(BB_SCK);
    bb_pin_take(BB_MOSI);
    bb_pin_take(BB_MISO);
    bb_pin_take(BB_CS);
    rt_pin_mode(BB_SCK, PIN_MODE_OUTPUT);
    rt_pin_mode(BB_MOSI, PIN_MODE_OUTPUT);
    rt_pin_mode(BB_CS, PIN_MODE_OUTPUT);
    rt_pin_mode(BB_MISO, PIN_MODE_INPUT);        /* 只读，别驱动 */
    rt_pin_write(BB_SCK, 0);                     /* mode0 空闲时钟低 */
    rt_pin_write(BB_CS, 1);                      /* 片选空闲高 */
    rt_thread_mdelay(2);

    /* 2) 先叫醒再读：芯片要是被带进"深度掉电(0xB9)"或卡在半条命令里，
     *    对 0x9F 一律不应答（读出来全 0，和"模块没接"一模一样）。
     *    0xAB 释放掉电、0x66+0x99 软复位 —— 这是 flash 卡住时的标准救法，
     *    放在这条"绕开外设"的诊断命令里最合适（外设通路坏了它也能跑）。*/
    rt_kprintf("  叫醒/复位: 0xAB(释放掉电) 0x66+0x99(软复位)\n");
    bb_send_cmd1(0xABu);
    rt_thread_mdelay(1);
    bb_send_cmd1(0x66u);
    bb_send_cmd1(0x99u);
    rt_thread_mdelay(2);

    /* 3) 连读 5 次 */
    for (k = 0; k < 5; k++) {
        rt_memset(id, 0, sizeof(id));
        bb_read_jedec(id);
        if (id[0] == 0xEFu && id[1] == 0x40u && id[2] == 0x17u) {
            pass++;
        }
        rt_kprintf("  第 %d 次: %02X %02X %02X  %s\n", k + 1, id[0], id[1], id[2],
                   (id[0] == 0xEFu && id[1] == 0x40u && id[2] == 0x17u) ? "= W25Q64 ✓"
                   : (id[0] == 0x00u && id[1] == 0x00u) ? "= 全 0（没人应答）"
                   : (id[0] == 0xFFu) ? "= 全 1（MISO 被拉高/悬空）" : "= 认不出来");
        rt_thread_mdelay(1);
    }
    rt_kprintf("  结果: 5 次里 %d 次读到 W25Q64\n", pass);
    if (pass == 5) {
        rt_kprintf("  ⇒ flash + 接线 + 供电都是好的，问题在 SPI 外设通路（drv_spi.c）\n");
    } else if (pass >= 1) {
        rt_kprintf("  ⇒ 位翻转也不稳 ⇒ 重点查接线/供电（杜邦线、VCC/GND、模块是否虚接）\n");
    } else {
        rt_kprintf("  ⇒ 位翻转一次都没读到 ⇒ 先查接线：SCK43/MOSI44/MISO45/CS46 + VCC/GND\n");
    }

    /* 3) 把 SPI 的路由装回去，别让后面的命令踩到空 */
    s31_spi_set_pins(0);
    /* 4) 再把时钟还给进来之前那一档（本命令为了稳会绑到 1 MHz）*/
    if (hz_before != 0u && dev_before[0] != '\0') {
        (void)s31_sfud_bind(dev_before, hz_before);
    }
    rt_kprintf("  （SPI2 的 matrix 路由已装回 43/44/45/46；时钟还原为 %u Hz）\n",
               (unsigned)s31_sfud_cur_hz());
}

/*---------------------------------------------------------------------------
 * sf info
 *-------------------------------------------------------------------------*/
static void sf_cmd_info(int argc, char **argv)
{
    sfud_flash *flash;
    sfud_err e;
    rt_uint8_t st = 0;
    int i;

    (void)argc; (void)argv;
    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }

    rt_kprintf("---- SFUD %s ----\n", SFUD_SW_VERSION);
    rt_kprintf("  名称      : %s\n", sf_chip_name(flash));
    rt_kprintf("  制造商    : %s（JEDEC ID %02X %02X %02X）\n",
               sf_mf_name(flash->chip.mf_id),
               flash->chip.mf_id, flash->chip.type_id, flash->chip.capacity_id);
    rt_kprintf("  绑定      : SPI 设备 \"%s\"，时钟 %u Hz\n",
               s31_sfud_bind_dev(), (unsigned)s31_sfud_cur_hz());
    rt_kprintf("  容量      : %u 字节 = %u KB = %u MB\n",
               (unsigned)flash->chip.capacity,
               (unsigned)(flash->chip.capacity / 1024u),
               (unsigned)(flash->chip.capacity / 1024u / 1024u));
    rt_kprintf("  写粒度    : %u 字节/次（跨页由 SFUD 自己切）\n",
               (unsigned)sf_write_gran(flash));
    rt_kprintf("  擦除粒度  : %u 字节\n", (unsigned)flash->chip.erase_gran);
    rt_kprintf("  地址模式  : %s\n", flash->addr_in_4_byte ? "4 字节" : "3 字节");

    if (flash->sfdp.available) {
        rt_kprintf("  SFDP      : 有（rev %u.%u，写粒度 %u B，擦除器 %u 种）\n",
                   (unsigned)flash->sfdp.major_rev, (unsigned)flash->sfdp.minor_rev,
                   (unsigned)flash->sfdp.write_gran, (unsigned)SFUD_SFDP_ERASE_TYPE_MAX_NUM);
        for (i = 0; i < (int)SFUD_SFDP_ERASE_TYPE_MAX_NUM; i++) {
            if (flash->sfdp.eraser[i].size != 0u) {
                rt_kprintf("              擦除器[%d]: 命令 0x%02X，%u 字节\n",
                           i, (unsigned)flash->sfdp.eraser[i].cmd,
                           (unsigned)flash->sfdp.eraser[i].size);
            }
        }
    } else {
        rt_kprintf("  SFDP      : 无（用的是内置型号表里的固定参数）\n");
    }

    /* 顺手把 0x5A 读到的 SFDP 头打出来：对不上时一眼看出是"表头就错"还是"解析错" */
    {
        rt_uint8_t hdr[8] = {0};
        rt_uint8_t tx[5];
        tx[0] = SFUD_CMD_READ_SFDP_REGISTER;    /* 0x5A */
        tx[1] = 0; tx[2] = 0; tx[3] = 0;        /* 地址 0x000000 */
        tx[4] = 0;                              /* 1 个空转字节 */
        e = flash->spi.wr(&flash->spi, tx, sizeof(tx), hdr, sizeof(hdr));
        if (e == SFUD_SUCCESS) {
            const char *magic = ((hdr[0] == 'S') && (hdr[1] == 'F') &&
                                 (hdr[2] == 'D') && (hdr[3] == 'P')) ? "SFDP" : "??（不是 SFDP 魔数）";
            rt_kprintf("  SFDP 头   : %02X %02X %02X %02X | rev %u.%u, %u 个参数头 -> %s\n",
                       hdr[0], hdr[1], hdr[2], hdr[3],
                       (unsigned)hdr[5], (unsigned)hdr[4], (unsigned)hdr[6], magic);
        } else {
            rt_kprintf("  SFDP 头   : 读失败 %s\n", sf_err_str(e));
        }
    }

    e = sfud_read_status(flash, &st);
    if (e == SFUD_SUCCESS) {
        rt_kprintf("  状态寄存器: 0x%02X（BUSY=%u WEL=%u BP=%u%u%u SRP=%u）\n",
                   (unsigned)st,
                   (unsigned)((st >> 0) & 1u), (unsigned)((st >> 1) & 1u),
                   (unsigned)((st >> 2) & 1u), (unsigned)((st >> 3) & 1u), (unsigned)((st >> 4) & 1u),
                   (unsigned)((st >> 7) & 1u));
    } else {
        rt_kprintf("  状态寄存器: 读失败 %s\n", sf_err_str(e));
    }
}

/*---------------------------------------------------------------------------
 * sf clk [hz]
 *-------------------------------------------------------------------------*/
static void sf_cmd_clk(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint32_t hz;

    if (argc < 2) {
        rt_kprintf("当前 SPI 时钟 %u Hz（默认 %u；`sf clk <hz>` 换一档试试）\n",
                   (unsigned)s31_sfud_cur_hz(), (unsigned)S31_SFUD_HZ_DEFAULT);
        return;
    }
    hz = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
    if (hz == 0u) {
        rt_kprintf("hz 不能是 0\n");
        return;
    }
    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }
    /* 重新 probe 等于"换频率重来"：会重读 JEDEC ID + SFDP，顺便验证新频率下通不通 */
    if (s31_sfud_probe(flash->name, s31_sfud_bind_dev(), hz) != RT_EOK) {
        rt_kprintf("  换到 %u Hz 后识别失败 —— 说明这档在这个模块/接线下不稳，退回 %u Hz\n",
                   (unsigned)hz, (unsigned)S31_SFUD_HZ_DEFAULT);
        s31_sfud_probe(flash->name, "flash0", S31_SFUD_HZ_DEFAULT);
        return;
    }
    rt_kprintf("  已在 %u Hz 下重新识别通过\n", (unsigned)hz);
}

/*---------------------------------------------------------------------------
 * sf read <addr> [len]
 *-------------------------------------------------------------------------*/
static void sf_cmd_read(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint32_t addr, len, i, j;
    rt_uint8_t *buf;
    sfud_err e;

    if (argc < 2) {
        rt_kprintf("用法: sf read <addr> [len]   （默认 len=%u，单次最多 %u）\n",
                   (unsigned)SF_DEFAULT_READ_LEN, (unsigned)SF_READ_MAX);
        return;
    }
    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }
    addr = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
    len  = (argc > 2) ? (rt_uint32_t)strtoul(argv[2], RT_NULL, 0) : SF_DEFAULT_READ_LEN;

    if (len == 0u || len > SF_READ_MAX) {
        rt_kprintf("len 取 1..%u\n", (unsigned)SF_READ_MAX);
        return;
    }
    if (addr >= flash->chip.capacity || addr + len > flash->chip.capacity) {
        rt_kprintf("越界: 0x%08X + %u > 容量 %u\n",
                   (unsigned)addr, (unsigned)len, (unsigned)flash->chip.capacity);
        return;
    }

    buf = (rt_uint8_t *)rt_malloc(len);
    if (buf == RT_NULL) {
        rt_kprintf("rt_malloc(%u) 失败（内部 RAM 不够？free 看看）\n", (unsigned)len);
        return;
    }

    e = sf_read_retry(flash, addr, len, buf);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("读失败: %s\n", sf_err_str(e));
        rt_free(buf);
        return;
    }

    rt_kprintf("%s: 0x%08X 起 %u 字节\n", sf_chip_name(flash), (unsigned)addr, (unsigned)len);
    rt_kprintf("Offset(h)  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
    for (i = 0; i < len; i += SF_HEXDUMP_WIDTH) {
        rt_kprintf("[%08X] ", (unsigned)(addr + i));
        for (j = 0; j < SF_HEXDUMP_WIDTH; j++) {
            if (i + j < len) {
                rt_kprintf("%02X ", buf[i + j]);
            } else {
                rt_kprintf("   ");
            }
        }
        rt_kprintf("|");
        for (j = 0; j < SF_HEXDUMP_WIDTH && (i + j) < len; j++) {
            rt_uint8_t c = buf[i + j];
            rt_kprintf("%c", (c >= 0x20u && c < 0x7Fu) ? (char)c : '.');
        }
        rt_kprintf("|\n");
    }
    rt_free(buf);
}

/*---------------------------------------------------------------------------
 * sf erase <addr> <len>
 *-------------------------------------------------------------------------*/
static void sf_cmd_erase(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint32_t addr, len, gran;
    rt_uint64_t t0;
    sfud_err e;

    if (argc < 3) {
        rt_kprintf("用法: sf erase <addr> <len>   （会按擦除粒度向上对齐，整片擦用 `sf erase 0 <容量>`）\n");
        return;
    }
    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }
    addr = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
    len  = (rt_uint32_t)strtoul(argv[2], RT_NULL, 0);
    gran = flash->chip.erase_gran;

    if (len == 0u) {
        rt_kprintf("len 不能是 0\n");
        return;
    }
    if (addr >= flash->chip.capacity || addr + len > flash->chip.capacity) {
        rt_kprintf("越界: 0x%08X + %u > 容量 %u\n",
                   (unsigned)addr, (unsigned)len, (unsigned)flash->chip.capacity);
        return;
    }
    /* 让"实际会擦掉哪一段"对用户透明：SFUD 内部是向上对齐的 */
    rt_kprintf("擦除 0x%08X..0x%08X（%u 字节，粒度 %u → 实际覆盖 0x%08X..0x%08X）...\n",
               (unsigned)addr, (unsigned)(addr + len), (unsigned)len, (unsigned)gran,
               (unsigned)(addr / gran * gran),
               (unsigned)(((addr + len + gran - 1u) / gran) * gran));

    t0 = s31_systimer_get_ticks();
    e = sfud_erase(flash, addr, len);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("擦除失败: %s\n", sf_err_str(e));
        return;
    }
    sf_rep("erase", len, sf_elapsed_us(t0));
}

/*---------------------------------------------------------------------------
 * sf write <addr> <hex...>   （先擦后写：sfud_erase_write）
 *
 * hex 两种写法，可混用：
 *   "DEADBEEF"   一串偶数个十六进制字符（推荐：一条命令能写 32 字节）
 *   AA BB CC     空格分开的字节（受 msh 的 FINSH_ARG_MAX 限制，最多 6 个）
 *-------------------------------------------------------------------------*/
static int sf_parse_hex(const char *sep, char *const *args, int nargs,
                        rt_uint8_t *out, rt_uint32_t out_max)
{
    int i;
    rt_uint32_t n = 0;

    if (sep != RT_NULL) {
        size_t slen = rt_strlen(sep), k;
        if ((slen & 1u) != 0u) {
            rt_kprintf("hex 串长度必须是偶数（每 2 个字符 1 字节），现在是 %u\n", (unsigned)slen);
            return -1;
        }
        if (slen / 2u > out_max) {
            rt_kprintf("hex 串太长（最多 %u 字节）\n", (unsigned)out_max);
            return -1;
        }
        for (k = 0; k < slen; k += 2u) {
            char b[3];
            char *end;
            unsigned long v;
            b[0] = sep[k]; b[1] = sep[k + 1u]; b[2] = '\0';
            v = strtoul(b, &end, 16);
            if (*end != '\0') {
                rt_kprintf("hex 串里有非十六进制字符: '%c%c'\n", b[0], b[1]);
                return -1;
            }
            out[n++] = (rt_uint8_t)v;
        }
        return (int)n;
    }

    for (i = 0; i < nargs; i++) {
        char *end;
        unsigned long v = strtoul(args[i], &end, 16);
        if (*end != '\0' || v > 0xFFul) {
            rt_kprintf("参数 '%s' 不是 0..FF 的十六进制字节\n", args[i]);
            return -1;
        }
        if (n >= out_max) {
            rt_kprintf("字节太多了（最多 %u）\n", (unsigned)out_max);
            return -1;
        }
        out[n++] = (rt_uint8_t)v;
    }
    return (int)n;
}

static void sf_cmd_write(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint8_t data[64];
    rt_uint32_t addr;
    int n;
    rt_uint64_t t0;
    sfud_err e;

    if (argc < 3) {
        rt_kprintf("用法: sf write <addr> <hex>       例: sf write 0x1000 \"DEADBEEF01020304\"\n"
                   "      sf write <addr> AA BB CC     （空格分开，最多 6 个字节）\n"
                   "  说明: **先擦后写**（擦除粒度 4KB）—— 同一扇区里已有的数据会被抹掉\n");
        return;
    }
    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }
    addr = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);

    /* argv[2] 以引号开头 → msh 已把引号剥掉了，所以这里只能"看内容"判断：
     * 没有空格、且长度是偶数、且全是 hex 字符 → 当 hex 串；否则按空格分开的字节列表 */
    if (argc == 3 && rt_strlen(argv[2]) > 2u && rt_strlen(argv[2]) % 2u == 0u) {
        n = sf_parse_hex(argv[2], RT_NULL, 0, data, sizeof(data));
    } else {
        n = sf_parse_hex(RT_NULL, &argv[2], argc - 2, data, sizeof(data));
    }
    if (n <= 0) {
        return;
    }
    if (addr >= flash->chip.capacity || addr + (rt_uint32_t)n > flash->chip.capacity) {
        rt_kprintf("越界: 0x%08X + %d > 容量 %u\n",
                   (unsigned)addr, n, (unsigned)flash->chip.capacity);
        return;
    }

    rt_kprintf("写 0x%08X 起 %d 字节（先擦 %u 字节对齐的扇区）...\n",
               (unsigned)addr, n, (unsigned)flash->chip.erase_gran);
    t0 = s31_systimer_get_ticks();
    e = sfud_erase_write(flash, addr, (size_t)n, data);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("写失败: %s\n", sf_err_str(e));
        return;
    }
    sf_rep("erase+write", (rt_uint32_t)n, sf_elapsed_us(t0));
    rt_kprintf("  写入内容: ");
    {
        int i;
        for (i = 0; i < n; i++) {
            rt_kprintf("%02X", data[i]);
        }
    }
    rt_kprintf("\n  用 `sf read 0x%08X %d` 读回来对一眼\n", (unsigned)addr, n);
}

/*---------------------------------------------------------------------------
 * sf status [volatile] [status]
 *-------------------------------------------------------------------------*/
static void sf_cmd_status(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint8_t st = 0;
    sfud_err e;

    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }

    if (argc >= 3) {
        bool vola = (strtoul(argv[1], RT_NULL, 0) != 0ul);
        rt_uint8_t val = (rt_uint8_t)strtoul(argv[2], RT_NULL, 0);
        e = sfud_write_status(flash, vola, val);
        if (e != SFUD_SUCCESS) {
            rt_kprintf("写状态寄存器失败: %s\n", sf_err_str(e));
            return;
        }
        rt_kprintf("已把状态寄存器写成 0x%02X（%s）\n", (unsigned)val,
                   vola ? "易失，掉电恢复" : "非易失，写进芯片");
    }

    e = sfud_read_status(flash, &st);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("读状态寄存器失败: %s\n", sf_err_str(e));
        return;
    }
    rt_kprintf("状态寄存器 = 0x%02X\n", (unsigned)st);
    rt_kprintf("  bit0 BUSY(WIP)   = %u   1 = 正在写/擦，此时只有读状态能应答\n",
               (unsigned)((st >> 0) & 1u));
    rt_kprintf("  bit1 WEL         = %u   1 = 写使能已置（写/擦前必须为 1）\n",
               (unsigned)((st >> 1) & 1u));
    rt_kprintf("  bit2..4 BP0..2   = %u%u%u 块保护（非 0 时那几块写不进/擦不掉）\n",
               (unsigned)((st >> 4) & 1u), (unsigned)((st >> 3) & 1u), (unsigned)((st >> 2) & 1u));
    rt_kprintf("  bit6 QE          = %u   1 = 四线使能（本 port 用不到）\n",
               (unsigned)((st >> 6) & 1u));
    rt_kprintf("  bit7 SRP0        = %u   1 = 状态寄存器被保护\n",
               (unsigned)((st >> 7) & 1u));
}

/*---------------------------------------------------------------------------
 * sf test <addr> [len] —— 完整自检
 *
 * 流程（每一步都打结果，"错在哪一步"一目了然）：
 *   ① 读回原内容做备份（RAM）          ② 擦除
 *   ③ 验擦干净（整段全 FF）            ④ 写花样（跨页、每字节不同）
 *   ⑤ 逐字节读回校验                   ⑥ 还原备份
 *-------------------------------------------------------------------------*/
static void sf_cmd_test(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint32_t addr, len, gran, i, errs, first_bad = 0xFFFFFFFFu;
    rt_uint8_t *wbuf = RT_NULL, *rbuf = RT_NULL, *bak = RT_NULL;
    rt_uint64_t t0;
    rt_uint32_t t_erase, t_write, t_read;
    sfud_err e;
    int ok = 1;

    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }
    if (argc < 2) {
        rt_kprintf("用法: sf test <addr> [len]   （默认 %u 字节；会先备份这段、测完还原）\n",
                   (unsigned)SF_TEST_LEN_DEFAULT);
        return;
    }
    addr = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
    len  = (argc > 2) ? (rt_uint32_t)strtoul(argv[2], RT_NULL, 0) : SF_TEST_LEN_DEFAULT;
    gran = flash->chip.erase_gran;

    /* 测试区向上对齐到擦除粒度：不然"还原备份"也保不住邻居的数据 */
    len = ((len + gran - 1u) / gran) * gran;
    addr = (addr / gran) * gran;

    if (len == 0u || len > SF_BUF_MAX) {
        rt_kprintf("len 取 1..%u（对齐后当前 %u）\n", (unsigned)SF_BUF_MAX, (unsigned)len);
        return;
    }
    if (addr + len > flash->chip.capacity) {
        rt_kprintf("越界: 0x%08X + %u > 容量 %u\n",
                   (unsigned)addr, (unsigned)len, (unsigned)flash->chip.capacity);
        return;
    }

    wbuf = (rt_uint8_t *)rt_malloc(len);
    rbuf = (rt_uint8_t *)rt_malloc(len);
    bak  = (rt_uint8_t *)rt_malloc(len);
    if (wbuf == RT_NULL || rbuf == RT_NULL || bak == RT_NULL) {
        rt_kprintf("rt_malloc 3×%u 失败 —— free 看看还剩多少（内部 RAM 就 460KB）\n",
                   (unsigned)len);
        goto out;
    }

    rt_kprintf("==== sf test: %s @0x%08X..0x%08X（%u 字节，%u 个 %u 字节扇区）====\n",
               sf_chip_name(flash), (unsigned)addr, (unsigned)(addr + len),
               (unsigned)len, (unsigned)(len / gran), (unsigned)gran);
    sf_show_clock();

    /* ① 备份 */
    e = sf_read_retry(flash, addr, len, bak);
    rt_kprintf("  ① 备份原内容      : %s\n", (e == SFUD_SUCCESS) ? "OK" : sf_err_str(e));
    if (e != SFUD_SUCCESS) {
        ok = 0;
        goto out;
    }
    /* 顺便报一下"这段里有多少非 FF 字节"—— 提醒用户这里本来有数据 */
    {
        rt_uint32_t used = 0;
        for (i = 0; i < len; i++) {
            if (bak[i] != 0xFFu) {
                used++;
            }
        }
        rt_kprintf("                     （这段里原有 %u 个非 0xFF 字节%s）\n",
                   (unsigned)used,
                   used ? "，测完会还原" : "");
    }

    /* ② 擦除 */
    t0 = s31_systimer_get_ticks();
    e = sfud_erase(flash, addr, len);
    t_erase = sf_elapsed_us(t0);
    rt_kprintf("  ② 擦除 %u 字节    : %s\n", (unsigned)len,
               (e == SFUD_SUCCESS) ? "OK" : sf_err_str(e));
    if (e != SFUD_SUCCESS) {
        ok = 0;
        goto out;
    }
    sf_rep("erase", len, t_erase);

    /* ③ 验擦干净 */
    e = sf_read_retry(flash, addr, len, rbuf);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("  ③ 验擦除后全 FF   : 读失败 %s\n", sf_err_str(e));
        ok = 0;
        goto out;
    }
    errs = 0; first_bad = 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        if (rbuf[i] != 0xFFu) {
            if (errs == 0u) {
                first_bad = i;
            }
            errs++;
        }
    }
    rt_kprintf("  ③ 验擦除后全 FF   : %s", errs ? "FAIL" : "OK");
    if (errs) {
        rt_kprintf("（%u 字节不是 0xFF，首个在 +%u = 0x%02X）",
                   (unsigned)errs, (unsigned)first_bad, rbuf[first_bad]);
        ok = 0;
    }
    rt_kprintf("\n");

    /* ④ 造花样：每字节都不同，且和地址相关（这样"整段写成同一个值"这类错会被抓出来）。
     *    最后再放一段"跨页"的连续递增数据：页边界处的翻页 bug 就靠它。*/
    for (i = 0; i < len; i++) {
        wbuf[i] = (rt_uint8_t)(0x5Au ^ (i * 7u) ^ (i >> 3) ^ (addr + i));
    }    for (i = 0; i + 256u <= len; i += 256u) {
        wbuf[i + 255u] = (rt_uint8_t)(0xA5u ^ (rt_uint8_t)(i >> 8));   /* 页尾 */
        if (i + 256u < len) {
            wbuf[i + 256u] = (rt_uint8_t)(0x3Cu ^ (rt_uint8_t)(i >> 8));/* 下一页页首 */
        }
    }

    t0 = s31_systimer_get_ticks();
    e = sfud_write(flash, addr, len, wbuf);
    t_write = sf_elapsed_us(t0);
    rt_kprintf("  ④ 写入 %u 字节    : %s\n", (unsigned)len,
               (e == SFUD_SUCCESS) ? "OK" : sf_err_str(e));
    if (e != SFUD_SUCCESS) {
        ok = 0;
        goto out;
    }
    sf_rep("write(含跨页)", len, t_write);

    /* ⑤ 读回逐字节校验 */
    t0 = s31_systimer_get_ticks();
    e = sf_read_retry(flash, addr, len, rbuf);
    t_read = sf_elapsed_us(t0);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("  ⑤ 读回校验        : 读失败 %s\n", sf_err_str(e));
        ok = 0;
        goto out;
    }
    errs = 0; first_bad = 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        if (rbuf[i] != wbuf[i]) {
            if (errs == 0u) {
                first_bad = i;
            }
            errs++;
        }
    }
    if (errs == 0u) {
        rt_kprintf("  ⑤ 读回逐字节校验  : OK（%u/%u 字节一致）\n",
                   (unsigned)len, (unsigned)len);
    } else {
        rt_kprintf("  ⑤ 读回逐字节校验  : FAIL（%u 字节不符，首个在 +%u：写 %02X 读 %02X）\n",
                   (unsigned)errs, (unsigned)first_bad, wbuf[first_bad], rbuf[first_bad]);
        ok = 0;
    }
    sf_rep("read(整段)", len, t_read);

    /* ⑥ 还原 */
    e = sfud_erase_write(flash, addr, len, bak);
    rt_kprintf("  ⑥ 还原原内容      : %s\n", (e == SFUD_SUCCESS) ? "OK" : sf_err_str(e));
    if (e != SFUD_SUCCESS) {
        ok = 0;
    } else {
        e = sf_read_retry(flash, addr, len, rbuf);
        if (e == SFUD_SUCCESS && rt_memcmp(rbuf, bak, len) != 0) {
            rt_kprintf("                     还原后校验不符！这一段的原数据已经丢了\n");
            ok = 0;
        }
    }

    rt_kprintf("==== sf test %s（擦 %u us / 写 %u us / 读 %u us）====\n",
               ok ? "PASS" : "FAIL",
               (unsigned)t_erase, (unsigned)t_write, (unsigned)t_read);

out:
    if (wbuf) { rt_free(wbuf); }
    if (rbuf) { rt_free(rbuf); }
    if (bak)  { rt_free(bak); }
}

/*---------------------------------------------------------------------------
 * sf bench <addr> <len> —— 速度基准
 *
 * ① 擦：整段一次 sfud_erase
 * ② 写：按页（write_gran=256B）顺序写满
 * ③ 读：逐 64 字节读（**当前 PIO 通路的真实速度**）
 * ④ 分块对照：同一段数据，用 64/256/1024/4096 字节的块各读一遍
 *    —— 这一栏是给"要不要给 drv_spi.c 上 DMA"做决策用的：
 *       如果 4096 的块也快不起来，说明瓶颈不在块长（那就是每字节的软件开销）。
 *-------------------------------------------------------------------------*/
static void sf_cmd_bench(int argc, char **argv)
{
    sfud_flash *flash;
    rt_uint32_t addr, len, gran, i, errs, r;
    rt_uint8_t *wbuf = RT_NULL, *rbuf = RT_NULL;
    rt_uint64_t t0;
    rt_uint32_t t_erase, t_write, t_read;
    sfud_err e;
    /* 事务尺寸对照用的档位：量的是"**一次交给驱动的字节数**"。
     * ≤64 走 PIO、>64 走 DMA —— 所以这条曲线会有一个明显的拐点，
     * 拐点右边就是 DMA 在干活（`drv_spi.c` 里 S31_SPI_PIO_MAX = 64）。*/
    static const rt_uint32_t chunks[] = {16u, 64u, 256u, 4096u};

    flash = sf_cur();
    if (flash == RT_NULL) {
        return;
    }
    if (argc < 3) {
        rt_kprintf("用法: sf bench <addr> <len>   （默认 len=%u；**只动这一段**）\n",
                   (unsigned)SF_BENCH_LEN_DEFAULT);
        return;
    }
    addr = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
    len  = (rt_uint32_t)strtoul(argv[2], RT_NULL, 0);
    gran = flash->chip.erase_gran;

    len  = ((len + gran - 1u) / gran) * gran;
    addr = (addr / gran) * gran;
    if (len == 0u || len > SF_BUF_MAX) {
        rt_kprintf("len 取 1..%u（对齐后当前 %u）\n", (unsigned)SF_BUF_MAX, (unsigned)len);
        return;
    }
    if (addr + len > flash->chip.capacity) {
        rt_kprintf("越界: 0x%08X + %u > 容量 %u\n",
                   (unsigned)addr, (unsigned)len, (unsigned)flash->chip.capacity);
        return;
    }

    wbuf = (rt_uint8_t *)rt_malloc(len);
    rbuf = (rt_uint8_t *)rt_malloc(len);
    if (wbuf == RT_NULL || rbuf == RT_NULL) {
        rt_kprintf("rt_malloc 2×%u 失败\n", (unsigned)len);
        goto out;
    }
    for (i = 0; i < len; i++) {
        wbuf[i] = (rt_uint8_t)(i * 31u + (i >> 5));
    }

    rt_kprintf("==== sf bench: %s @0x%08X..0x%08X（%u 字节）====\n",
               sf_chip_name(flash), (unsigned)addr, (unsigned)(addr + len), (unsigned)len);
    sf_show_clock();
    rt_kprintf("  ⚠️ 这一段的原数据会被覆盖（不做备份；要保护就先 `sf read` 存下来）\n");

    /* ① 擦 */
    t0 = s31_systimer_get_ticks();
    e = sfud_erase(flash, addr, len);
    t_erase = sf_elapsed_us(t0);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("擦除失败: %s\n", sf_err_str(e));
        goto out;
    }
    sf_rep("erase", len, t_erase);

    /* ② 写（按页，模拟真实写文件的姿势）*/
    {
        rt_uint32_t pg = sf_write_gran(flash);
        t0 = s31_systimer_get_ticks();
        for (i = 0; i < len; i += pg) {
            rt_uint32_t n = pg;
            if (i + n > len) {
                n = len - i;
            }
            e = sfud_write(flash, addr + i, n, wbuf + i);
            if (e != SFUD_SUCCESS) {
                rt_kprintf("写失败 @+%u: %s\n", (unsigned)i, sf_err_str(e));
                goto out;
            }
        }
        t_write = sf_elapsed_us(t0);
        {
            char label[40];
            rt_snprintf(label, sizeof(label), "write(按 %uB 页)", (unsigned)pg);
            sf_rep(label, len, t_write);
        }
    }

    /* ③ 读：一次一整段（内部会按 64 字节切）*/
    t0 = s31_systimer_get_ticks();
    e = sf_read_retry(flash, addr, len, rbuf);
    t_read = sf_elapsed_us(t0);
    if (e != SFUD_SUCCESS) {
        rt_kprintf("读失败: %s\n", sf_err_str(e));
        goto out;
    }
    errs = 0;
    for (i = 0; i < len; i++) {
        if (rbuf[i] != wbuf[i]) {
            errs++;
        }
    }
    rt_kprintf("  读回校验: %s（%u 字节不符）\n", errs ? "FAIL" : "OK", (unsigned)errs);
    sf_rep("read(整段,内部切 64B)", len, t_read);

    /* ④ 事务尺寸对照：把**移植层的切块大小**从 64 往下调，各读一遍。
     *    ⚠️ 第一版这里只是换上层 `sfud_read` 的调用长度 —— 那是**假的**：
     *       移植层内部固定按 64 切，上层调多大结果都一样（量出来几条几乎重合）。
     *       现在调的是 `s31_sfud_set_chunk()`，是真的在改 SPI 事务尺寸。
     *    这一栏的用处：相邻两档的差值 ≈ 每次事务的固定开销（配寄存器 + 忙等），
     *    乘上"读这段要发多少次事务"就知道还有多少优化空间。*/
    rt_kprintf("  -- SPI 事务尺寸对照（改的是移植层切块大小，同一段数据每档 3 轮取最快）--\n");
    for (r = 0; r < sizeof(chunks) / sizeof(chunks[0]); r++) {
        rt_uint32_t chunk = chunks[r], best = 0xFFFFFFFFu, round;
        if (chunk > len) {
            continue;
        }
        s31_sfud_set_chunk(chunk);
        for (round = 0; round < 3u; round++) {
            rt_uint32_t us;
            t0 = s31_systimer_get_ticks();
            if (sf_read_retry(flash, addr, len, rbuf) != SFUD_SUCCESS) {
                rt_kprintf("    事务尺寸 %u 读失败\n", (unsigned)chunk);
                s31_sfud_set_chunk(0u);
                goto out;
            }
            us = sf_elapsed_us(t0);
            if (us < best) {
                best = us;
            }
        }
        {
            char label[40];
            rt_snprintf(label, sizeof(label), "每次事务 %u B", (unsigned)chunk);
            sf_rep(label, len, best);
        }
    }
    s31_sfud_set_chunk(0u);                     /* 恢复默认：不切块（整段交给驱动）*/

    rt_kprintf("  ⚠️ ≤64 字节走 PIO（16 字 FIFO），>64 走 DMA。参考工程用 IDF 的\n");
    rt_kprintf("     DMA 单事务读 4KB 能到 20MHz 线速的 98.7%%；要让这儿也接近线速，\n");
    rt_kprintf("     得给 drv_spi.c 上 DMA（见 README §5.6）\n");

out:
    if (wbuf) { rt_free(wbuf); }
    if (rbuf) { rt_free(rbuf); }
}

/*---------------------------------------------------------------------------
 * 帮助 + 分发
 *-------------------------------------------------------------------------*/
static void sf_usage(void)
{
    rt_kprintf("SFUD（Serial Flash Universal Driver %s）—— 外接 SPI flash\n", SFUD_SW_VERSION);
    rt_kprintf("用法: sf <子命令> ...\n");
    rt_kprintf("  sf probe [spi_dev] [hz]   识别芯片（默认 flash0 / %u Hz）\n",
               (unsigned)S31_SFUD_HZ_DEFAULT);
    rt_kprintf("  sf info                   芯片参数 / SFDP / 状态寄存器\n");
    rt_kprintf("  sf dbg [hz]               裸事务诊断：0x9F/0x90/0x5A/0x05/0x03 的原始字节\n");
    rt_kprintf("  sf clk [hz]               看或换 SPI 时钟（换完会重识别一遍）\n");
    rt_kprintf("  sf read <addr> [len]      读并 dump（默认 %u 字节，最多 %u）\n",
               (unsigned)SF_DEFAULT_READ_LEN, (unsigned)SF_READ_MAX);
    rt_kprintf("  sf write <addr> <hex>     先擦后写；hex 例 \"DEADBEEF\" 或 AA BB CC\n");
    rt_kprintf("  sf erase <addr> <len>     擦除（按 4KB 扇区对齐）\n");
    rt_kprintf("  sf status [vol] [val]     读/写状态寄存器\n");
    rt_kprintf("  sf test <addr> [len]      完整自检：备份→擦→验 FF→写→读回校验→还原\n");
    rt_kprintf("  sf bench <addr> <len>     速度基准：擦/写/读 + 分块长度对照\n");
    rt_kprintf("接线: SCK=GPIO43(J2-17) MOSI=GPIO44(J2-18) MISO=GPIO45(J2-15) CS=GPIO46(J2-16)\n");
}

static void sf(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*fn)(int, char **);
        int min_args;               /* 含子命令名本身 */
    } tbl[] = {
        { "probe",  sf_cmd_probe,  1 },
        { "info",   sf_cmd_info,   1 },
        { "dbg",    sf_dbg,        1 },
        { "bb",     sf_bb,         1 },
        { "clk",    sf_cmd_clk,    1 },
        { "read",   sf_cmd_read,   2 },
        { "write",  sf_cmd_write,  3 },
        { "erase",  sf_cmd_erase,  3 },
        { "status", sf_cmd_status, 1 },
        { "test",   sf_cmd_test,   2 },
        { "bench",  sf_cmd_bench,  3 },
    };
    rt_uint32_t i;

    if (argc < 2) {
        sf_usage();
        return;
    }
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (rt_strcmp(argv[1], tbl[i].name) == 0) {
            if (argc < tbl[i].min_args) {
                sf_usage();
                return;
            }
            tbl[i].fn(argc - 1, argv + 1);
            return;
        }
    }
    if (rt_strcmp(argv[1], "help") == 0 || rt_strcmp(argv[1], "-h") == 0) {
        sf_usage();
        return;
    }
    rt_kprintf("未知子命令 \"%s\"\n", argv[1]);
    sf_usage();
}
MSH_CMD_EXPORT(sf, SFUD SPI flash: probe/info/read/write/erase/test/bench);
