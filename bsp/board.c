/*===========================================================================
 * board.c -- BSP 板级初始化
 *
 * 启动顺序（由 src/components.c 的 rtthread_startup 驱动）：
 *   startup.S → entry() → rt_hw_interrupt_disable() → **rt_hw_board_init()**
 *            → rt_show_version() → rt_system_timer_init() → rt_system_scheduler_init()
 *            → rt_application_init()（建 main 线程）→ rt_system_scheduler_start()
 *   main 线程里 rt_components_init() 遍历 .rti_fn 的 3..6 级
 *            → INIT_DEVICE_EXPORT(systick 开中断) → ... → INIT_APP_EXPORT(finsh_system_init) → msh 起来
 *   ⚠️ 0/1 级（INIT_BOARD_EXPORT）在本 port **不会执行**：本文件没有调用
 *      rt_components_board_init()，板级初始化就在下面的 rt_hw_board_init() 里显式做。
 *      （实测：nm 里没有 rt_components_board_init，也没人引用 __rt_init_rti_start）
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include "s31_regs.h"

void s31_usj_hw_init(void);
void s31_systick_hw_init(void);
int  s31_clk_init(void);
void s31_clk_report(void);
int  s31_psram_init(void);
int  s31_psram_ready(void);
rt_uint32_t s31_psram_size(void);

/* 链接脚本给的堆范围：__bss_end .. 中断栈底 */
extern char __heap_start[];
extern char __heap_end[];

/* 调试：把 RT-Thread 自动初始化表摊开（排查"某个 INIT_*_EXPORT 没执行"） */
extern char __rti_fn_start[];
extern char __rti_fn_end[];
extern const void *__rt_init_rti_start;
extern const void *__rt_init_rti_board_start;
extern const void *__rt_init_rti_board_end;
extern const void *__rt_init_rti_end;

/* 调试开关：把 RT-Thread 自动初始化表摊开（排查"某个 INIT_*_EXPORT 没执行"）。
 * 平时关掉，开机日志干净；出问题时置 1 重编。 */
#define S31_DEBUG_INIT_TABLE 0

#if S31_DEBUG_INIT_TABLE
static void s31_dump_init_table(void)
{
    rt_kprintf("[ini] table 0x%08x .. 0x%08x (%u entries)\n",
               (unsigned)(rt_ubase_t)__rti_fn_start, (unsigned)(rt_ubase_t)__rti_fn_end,
               (unsigned)((__rti_fn_end - __rti_fn_start) / 4));
    rt_kprintf("[ini] sentinels: start=%08x board_start=%08x board_end=%08x end=%08x\n",
               (unsigned)(rt_ubase_t)&__rt_init_rti_start,
               (unsigned)(rt_ubase_t)&__rt_init_rti_board_start,
               (unsigned)(rt_ubase_t)&__rt_init_rti_board_end,
               (unsigned)(rt_ubase_t)&__rt_init_rti_end);
    const void **p;
    for (p = (const void **)__rti_fn_start; p < (const void **)__rti_fn_end; p++) {
        rt_uint32_t a = (rt_uint32_t)(rt_ubase_t)*p;
        /* 粗判"像不像合法函数指针"：落在镜像区间内 */
        rt_kprintf("[ini]   @%08x -> %08x %s\n", (unsigned)(rt_ubase_t)p, (unsigned)a,
                   (a >= 0x2F000000u && a < 0x2F07AFC0u) ? "" : "<-- SUSPECT");
    }
}
#endif /* S31_DEBUG_INIT_TABLE */

/* ---- 复位原因（真凶判别用）---------------------------------------------
 * `LP_AONCLKRST_HPCORE0_RESET_CAUSE_REG`(0x20701030)：bit0=flag、bit[6:1]=cause。
 * 枚举（`soc/esp32s31/register/soc/lp_clkrst_struct.h`）：
 *   0x1 POR / 0x3 数字系统软复位 / 0x5 PMU 掉电 / 0x7 HP WDT0 / 0x8 HP WDT1 /
 *   0x9 HP 系统被 LP 看门狗复位 / 0xb HP 核被 HP 看门狗复位 / 0xc HP 核软复位 /
 *   0xd HP 核被 LP 看门狗复位 / 0xf 欠压 / 0x10 LP 看门狗整片复位 / 0x12 超级看门狗 /
 *   0x13 毛刺 / 0x14 eFuse CRC / **0x16 USB-JTAG 请求** / **0x17 USB-UART(CDC) 请求** /
 *   0x18 JTAG / 0x1a HP 核 lockup。
 * 🚨 ROM 打印的 `rst:0x..` **可能是上次留下的旧值**（该寄存器要写 bit30 才清），
 *    所以这里**先打印再清**：下次开机看到的就是本次真实原因。*/
#define S31_LP_AONCLKRST_BASE   0x20701000u
#define S31_HPCORE0_RST_CAUSE   (S31_LP_AONCLKRST_BASE + 0x30u)
#define S31_HPCORE1_RST_CAUSE   (S31_LP_AONCLKRST_BASE + 0x34u)
#define S31_LPCORE_RST_CAUSE    (S31_LP_AONCLKRST_BASE + 0x38u)
#define S31_RST_CAUSE_CLR       (1u << 30)

static const char *s31_rst_cause_str(rt_uint32_t c)
{
    switch (c) {
    case 0x01: return "POR(上电)";
    case 0x03: return "数字系统软复位";
    case 0x05: return "PMU 掉电复位";
    case 0x07: return "HP 看门狗0";
    case 0x08: return "HP 看门狗1";
    case 0x09: return "HP 系统被 LP 看门狗复位";
    case 0x0b: return "HP 核被 HP 看门狗复位";
    case 0x0c: return "HP 核软复位";
    case 0x0d: return "HP 核被 LP 看门狗复位";
    case 0x0f: return "欠压(BROWN OUT)";
    case 0x10: return "LP 看门狗整片复位";
    case 0x12: return "超级看门狗";
    case 0x13: return "毛刺复位";
    case 0x14: return "eFuse CRC 错";
    case 0x16: return "USB-JTAG 请求";
    case 0x17: return "USB-UART(CDC) 请求";
    case 0x18: return "JTAG 复位";
    case 0x1a: return "HP 核 lockup";
    default:   return "未定义";
    }
}

void s31_reset_cause_report(void)
{
    rt_uint32_t c0 = S31_REG32(S31_HPCORE0_RST_CAUSE);
    rt_uint32_t c1 = S31_REG32(S31_HPCORE1_RST_CAUSE);
    rt_uint32_t cl = S31_REG32(S31_LPCORE_RST_CAUSE);

    rt_kprintf("  reset : core0=0x%02x %s\n", (unsigned)((c0 >> 1) & 0x3Fu),
               s31_rst_cause_str((c0 >> 1) & 0x3Fu));
    rt_kprintf("          core1=0x%02x lpcore=0x%02x  raw=%08x/%08x/%08x\n",
               (unsigned)((c1 >> 1) & 0x3Fu), (unsigned)((cl >> 1) & 0x3Fu),
               (unsigned)c0, (unsigned)c1, (unsigned)cl);

    /* 清掉，保证下次读到的是"本次"的原因 */
    S31_REG32(S31_HPCORE0_RST_CAUSE) = S31_RST_CAUSE_CLR;
    S31_REG32(S31_HPCORE1_RST_CAUSE) = S31_RST_CAUSE_CLR;
    S31_REG32(S31_LPCORE_RST_CAUSE)  = S31_RST_CAUSE_CLR;
}

void rt_hw_board_init(void)
{
    /* 1. 控制台（后面的 rt_kprintf 都靠它；此时还没有 console 设备，
     *    kservice.c 会回退到 rt_hw_console_output） */
    s31_usj_hw_init();

    /* 2. 中断分发表初始化：**只能调一次**（trap_handler.c 的实现会把表清空重建）
     *    → 放在这里，两个驱动（systick / usj-rx）之后只做 install */
    rt_hw_interrupt_init();

    /* 3. 提速：CPU 40MHz(XTAL) → 320MHz(CPLL)。失败会自动退回 40MHz 并打日志。
     *    放在这里是为了让后面的开机横幅/tick 都在最终频率下跑。 */
    s31_clk_init();

    /* 2. 堆：用链接脚本划出来的那段（不是静态数组，能吃到 ~460KB） */
#if defined(RT_USING_USER_MAIN) && defined(RT_USING_HEAP)
    rt_system_heap_init(__heap_start, __heap_end);
#endif

    /* 3. tick 硬件（中断在 INIT_DEVICE_EXPORT 里才开） */
    s31_systick_hw_init();

    /* 4. 频率自检：**必须**在 systick 硬件初始化之后（测量要读 SYSTIMER，
     *    而 SYSTIMER 的时钟门控是上一步开的；提前读会死等 VALUE_VALID） */
    s31_clk_report();

    /* 5. PSRAM（16MB 8 线 DDR @200MHz，窗口 0x50000000）
     *    放在 systick 之后是因为里面的微秒延时读 SYSTIMER；
     *    放在堆初始化之后是因为它自己要开 cache（D-cache 对堆也是加速）。
     *    ⚠️ 失败**不中止**：PSRAM 只是"额外内存"，起不来也该能进 msh 看日志
     *       （boot_msc_s31 里它是必需品，这里的定位不一样）。*/
    s31_psram_init();

#ifdef RT_USING_CONSOLE
    rt_kprintf("\n");
    rt_kprintf("=============================================\n");
    rt_kprintf("  RT-Thread Nano on ESP32-S31 (no IDF, no idf.py)\n");
    rt_kprintf("  heap  : 0x%08x .. 0x%08x (%u KB)\n",
               (rt_uint32_t)(rt_ubase_t)__heap_start, (rt_uint32_t)(rt_ubase_t)__heap_end,
               (unsigned)(((char *)__heap_end - (char *)__heap_start) / 1024));
    rt_kprintf("  tick  : SYSTIMER @%u Hz, %u ticks/tick, CLIC id %d\n",
               (unsigned)S31_SYSTIMER_HZ,
               (unsigned)(S31_SYSTIMER_HZ / BSP_S31_SYSTICK_HZ),
               S31_CLIC_EXT_OFFSET);
    rt_kprintf("  psram : %s\n",
               s31_psram_ready()
                   ? "OK — 16MB @0x50000000（psram_info / psram_speed 看详情）"
                   : "FAIL — 看上面的 [psram] 行（其它功能不受影响）");
    s31_reset_cause_report();
    rt_kprintf("=============================================\n");
#if S31_DEBUG_INIT_TABLE
    s31_dump_init_table();
#endif
#endif /* RT_USING_CONSOLE */
}
