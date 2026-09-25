/*===========================================================================
 * drv_systick.c -- RT-Thread 系统 tick：SYSTIMER TARGET0 + CLIC
 *
 * SYSTIMER 时钟固定 XTAL 40MHz / 2.5 = 16MHz（esp_hw_support/port/esp32s31/systimer.c:10）
 *   → 1ms = 16000 个计数
 *
 * 中断通路（S31 是 CLIC，不是老式 interrupt matrix）：
 *   外设源 33 (ETS_SYSTIMER_TARGET0) --路由矩阵(0x20585000+4*33)--> CLIC ID 16
 *   CLIC ID 16：ATTR=电平/非向量，CTL=优先级1，IE=1
 *   阈值用 mintthresh CSR（S31 是标准 CLIC，interrupt_reg.h:28）
 *   trap 入口 = SW_handler（linker 里保证 64 字节对齐）
 *
 * ⚠️ tick 中断**必须**等 rt_system_timer_init() 之后再开：否则 rt_tick_increase()
 *    会去遍历还没初始化的定时器链表。所以硬件初始化在 rt_hw_board_init()，
 *    开中断放在 INIT_BOARD_EXPORT（rt_components_init() 里最先跑的那批）。
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include "s31_regs.h"

void s31_usj_tx_pump(void);       /* bsp/drv_usj.c：把控制台发送缓冲灌进 TX FIFO */

#define S31_TICK_CLIC_ID   (S31_CLIC_EXT_OFFSET + 0)      /* 16，外部中断第一个口 */
#define S31_TICK_TICKS     (S31_SYSTIMER_HZ / BSP_S31_SYSTICK_HZ)

static void s31_tick_handler(int irq, void *param)
{
    (void)irq;
    (void)param;
    S31_REG32(S31_SYSTIMER_INT_CLR) = S31_SYSTIMER_T0_INT;   /* 清挂起（电平触发必须清） */
    s31_usj_tx_pump();                                        /* 1ms 一次把控制台缓冲灌出去 */
    rt_tick_increase();
}

/* 只动硬件，不开中断。
 * 顺序照 IDF 的 FreeRTOS tick（freertos/port_systick.c:74-114）：
 *   开时钟 → 开 unit → **先把计数器清零** → 配周期/目标 → COMP_LOAD 生效 → 最后 arm 比较器
 * 🚨 踩过的坑：先 arm（TARGET0_WORK_EN）再配目标，且不清计数器，
 *    结果只中断一次（周期模式没生效）——顺序很重要。 */
void s31_systick_hw_init(void)
{
    /* 1. 时钟门控（hp_sys_clkrst_reg.h:2962：bit0 APB_CLK_EN、bit4 CLK_EN） */
    S31_REG32(S31_SYSTIMER_CTRL0) |= (S31_SYSTIMER_APB_CLK_EN | S31_SYSTIMER_CLK_EN_BIT);

    /* 2. 寄存器时钟 + unit0 工作，但**先不 arm target0** */
    S31_REG32(S31_SYSTIMER_CONF) |= (S31_SYSTIMER_CLK_EN |
                                     S31_SYSTIMER_UNIT0_WORK_EN);
    S31_REG32(S31_SYSTIMER_CONF) &= ~S31_SYSTIMER_TARGET0_WORK_EN;

    /* 3. 计数器清零（UNIT0_LOAD_HI/LO → UNIT0_LOAD 生效） */
    S31_REG32(S31_SYSTIMER_UNIT0_LOAD_HI) = 0;
    S31_REG32(S31_SYSTIMER_UNIT0_LOAD_LO) = 0;
    S31_REG32(S31_SYSTIMER_UNIT0_LOAD) = 1;

    /* 4. 目标值 = 16000（1ms）+ 周期模式（不开周期模式只会中断一次） */
    S31_REG32(S31_SYSTIMER_TARGET0_HI) = 0;
    S31_REG32(S31_SYSTIMER_TARGET0_LO) = S31_TICK_TICKS;
    S31_REG32(S31_SYSTIMER_TARGET0_CONF) =
        (S31_TICK_TICKS & 0x03FFFFFFu) |            /* PERIOD[25:0] = 1ms */
        S31_SYSTIMER_TARGET0_PERIOD_MODE |          /* PERIOD_MODE = 1 */
        0u;                                         /* TIMER_UNIT_SEL = 0 → UNIT0 */
    S31_REG32(S31_SYSTIMER_COMP0_LOAD) = 1;         /* 让目标值生效 */

    /* 5. 清掉可能已挂起的中断，然后才 arm */
    S31_REG32(S31_SYSTIMER_INT_CLR) = S31_SYSTIMER_T0_INT;
    S31_REG32(S31_SYSTIMER_CONF) |= S31_SYSTIMER_TARGET0_WORK_EN;

    /* 6. 路由：源 33 → CLIC ID 16（interrupt_clic_ll.h:41 的写法） */
    S31_REG32(S31_INTR0_BASE + 4 * S31_ETS_SYSTIMER_TARGET0) = S31_TICK_CLIC_ID;

    /* 7. CLIC ID 16：清挂起、电平触发、非向量、优先级 1、使能 */
    S31_REG8(S31_CLIC_IP(S31_TICK_CLIC_ID))   = 0;
    S31_REG8(S31_CLIC_ATTR(S31_TICK_CLIC_ID)) = 0;
    S31_REG8(S31_CLIC_CTL(S31_TICK_CLIC_ID))  = S31_CLIC_CTL_PRIO(1);
    S31_REG8(S31_CLIC_IE(S31_TICK_CLIC_ID))   = 1;
}

/* 读 SYSTIMER 计数（必须走 UPDATE/VALUE_VALID 握手 + 读两遍防撕裂，
 * 直接读 VALUE_LO 只会得到 0 —— systimer_hal_get_counter_value 的做法） */
rt_uint64_t s31_systimer_get_ticks(void)
{
    rt_uint32_t hi, lo, lo2;
    volatile rt_uint32_t guard = 200000u;      /* 🚨 必须带超时：systimer 时钟没开时
                                                * VALUE_VALID 永远不置位，无上限等待会把
                                                * 系统挂死在这儿（踩过：升频后"卡在切换后"） */

    S31_REG32(S31_SYSTIMER_UNIT0_OP) = S31_SYSTIMER_UNIT0_OP_UPDATE;
    while (!(S31_REG32(S31_SYSTIMER_UNIT0_OP) & S31_SYSTIMER_UNIT0_VALUE_VALID) && --guard) {
    }
    if (guard == 0) {
        return 0;                              /* systimer 没起来（时钟门控未开）*/
    }
    do {
        lo  = S31_REG32(S31_SYSTIMER_UNIT0_VALUE_LO);
        hi  = S31_REG32(S31_SYSTIMER_UNIT0_VALUE_HI);
        lo2 = S31_REG32(S31_SYSTIMER_UNIT0_VALUE_LO);
    } while (lo != lo2);

    return ((rt_uint64_t)hi << 32) | lo;
}

/*===========================================================================
 * 调试用：把 tick 通路上所有关键寄存器打出来（一次启动就能定位是哪一环断了）
 *===========================================================================*/
static rt_uint32_t rd_mstatus(void) { rt_uint32_t v; __asm__ volatile("csrr %0, mstatus" : "=r"(v)); return v; }
static rt_uint32_t rd_mtvec(void)   { rt_uint32_t v; __asm__ volatile("csrr %0, mtvec"   : "=r"(v)); return v; }
static rt_uint32_t rd_mie(void)     { rt_uint32_t v; __asm__ volatile("csrr %0, mie"     : "=r"(v)); return v; }
static rt_uint32_t rd_mip(void)     { rt_uint32_t v; __asm__ volatile("csrr %0, mip"     : "=r"(v)); return v; }
static rt_uint32_t rd_mintthresh(void) { rt_uint32_t v; __asm__ volatile("csrr %0, 0x347" : "=r"(v)); return v; }

void s31_systick_dump(void)
{
    rt_kprintf("[cli] mstatus=%08x mtvec=%08x mie=%08x mip=%08x mintthresh=%08x\n",
               (unsigned)rd_mstatus(), (unsigned)rd_mtvec(), (unsigned)rd_mie(),
               (unsigned)rd_mip(), (unsigned)rd_mintthresh());
    rt_kprintf("[cli] id%d: ip=%02x ie=%02x attr=%02x ctl=%02x   route(src33)=%08x\n",
               S31_TICK_CLIC_ID,
               S31_REG8(S31_CLIC_IP(S31_TICK_CLIC_ID)),
               S31_REG8(S31_CLIC_IE(S31_TICK_CLIC_ID)),
               S31_REG8(S31_CLIC_ATTR(S31_TICK_CLIC_ID)),
               S31_REG8(S31_CLIC_CTL(S31_TICK_CLIC_ID)),
               (unsigned)S31_REG32(S31_INTR0_BASE + 4 * S31_ETS_SYSTIMER_TARGET0));

    rt_kprintf("[sy ] ctrl0=%08x conf=%08x tgt0=%08x/%08x conf0=%08x comp0_load=%d unit0_load=%d\n",
               (unsigned)S31_REG32(S31_SYSTIMER_CTRL0),
               (unsigned)S31_REG32(S31_SYSTIMER_CONF),
               (unsigned)S31_REG32(S31_SYSTIMER_TARGET0_HI),
               (unsigned)S31_REG32(S31_SYSTIMER_TARGET0_LO),
               (unsigned)S31_REG32(S31_SYSTIMER_TARGET0_CONF),
               (int)S31_REG32(S31_SYSTIMER_COMP0_LOAD),
               (int)S31_REG32(S31_SYSTIMER_UNIT0_LOAD));
    rt_kprintf("[sy ] int_ena=%08x int_raw=%08x int_st=%08x\n",
               (unsigned)S31_REG32(S31_SYSTIMER_INT_ENA),
               (unsigned)S31_REG32(S31_SYSTIMER_INT_RAW),
               (unsigned)S31_REG32(S31_SYSTIMER_INT_ST));

    /* 计数器在不在走（UPDATE/VALID 握手，见 s31_systimer_get_ticks） */
    rt_uint64_t a = s31_systimer_get_ticks();
    for (volatile int i = 0; i < 2000000; i++) { }
    rt_uint64_t b = s31_systimer_get_ticks();
    rt_kprintf("[sy ] ticks: %u -> %u  (delta=%d, expect ~%d us worth)\n",
               (unsigned)a, (unsigned)b, (int)(b - a), 20000);
}

/* CLIC 的向量表（mtvt CSR = 0x307）。
 * 我们的中断全是 SHV=0（非向量）→ 走 mtvec 基址，这张表其实用不到；
 * 但万一某个中断被误设成向量（IDF 默认把外设线都设成 vectored），
 * CPU 会跳 mtvt + 4*id —— 填满 SW_handler 就不会跳到野地里去。 */
extern void SW_handler(void);
static void *const s31_mtvt[48] __attribute__((aligned(64))) = {
    [0 ... 47] = (void *)SW_handler
};

/* 开中断的时机 —— 这里有坑，记下来：
 *   ❌ INIT_BOARD_EXPORT（1 级）在这里**不会执行**：board 级要 BSP 自己在
 *      rt_hw_board_init() 里调 rt_components_board_init() 才跑
 *      （components.c:110 的 rt_components_init() 只遍历 board_end..rti_end，即 2~6 级）。
 *      而我们也**不能**在 rt_hw_board_init() 里开 tick：那时 rt_system_timer_init()
 *      还没执行，第一个 tick 一来 rt_timer_check() 就会去遍历未初始化的定时器链表。
 *   ✅ INIT_DEVICE_EXPORT（3 级）正好：它在 main 线程的 rt_components_init() 里跑，
 *      定时器链表和调度器都已就绪，而 finsh（6 级）还没起来。 */
static int s31_systick_start(void)
{
    /* 分发表已在 rt_hw_board_init() 里初始化过一次，这里只装自己的处理函数 */
    rt_hw_interrupt_install(S31_TICK_CLIC_ID, s31_tick_handler, RT_NULL, "systick");

    S31_REG32(S31_SYSTIMER_INT_ENA) = S31_SYSTIMER_T0_INT;

    /* 向量表基址（64 字节对齐），见上面注释 */
    __asm__ volatile ("csrw 0x307, %0" :: "r"((uint32_t)(uintptr_t)s31_mtvt));

    /* 阈值 = 0（允许全部优先级）：S31 用标准 mintthresh CSR 0x347。
     * ⚠️ CLIC 每个中断的优先级复位默认是 0x1F，而阈值 0x1F 恰好屏蔽 level 0
     *    → 优先级必须显式写成 ≥1（我们在 hw_init 里写的是 level 1）。 */
    __asm__ volatile ("csrw 0x347, %0" :: "r"((uint32_t)S31_MINTTHRESH_ALL));
    __asm__ volatile ("csrs mstatus, 8");      /* MIE = 1 */

    return 0;
}
INIT_DEVICE_EXPORT(s31_systick_start);
