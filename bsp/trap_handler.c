/*===========================================================================
 * trap_handler.c -- 中断/异常分发（覆盖 libcpu/risc-v/common/trap_common.c 的弱符号）
 *
 * 为什么不直接用 trap_common.c：
 *   1. 它的异常分支只打寄存器快照，**不打 mcause/mepc/mtval** —— 排查时最需要的
 *      三个数反而没有；而且打完就返回，故障指令会再次执行 → 无限刷屏
 *      （实测 8 秒刷了 2.3MB）。
 *   2. 它的中断表只有 32 项且按 mcause & 0x1F 取（CLIC ID 16~31 够用），
 *      但我们要在未注册中断时打一行带 ID 的日志，方便定位。
 *
 * CLIC 的 mcause 编码（IDF riscv/include/esp_private/vectors_const.h:9-25）：
 *   bit31          = 1 中断 / 0 异常
 *   reason[11:0]   = 中断号（就是 CLIC ID）
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include "s31_regs.h"          /* S31_REG8 / S31_CLIC_IE：未注册中断时要把那路关掉 */

#define S31_ISR_MAX   32

struct s31_isr_entry
{
    rt_isr_handler_t handler;
    void            *param;
};

static struct s31_isr_entry s31_isr_table[S31_ISR_MAX];

static void s31_default_isr(int vector, void *param)
{
    static rt_uint32_t spammed;
    (void)param;

    /* 🚨 未注册的中断**不能只打一行就返回**：外设的中断标志没人清 → 电平触发下
     *    会立刻再进来 → 死循环刷屏，而且这时候 shell 根本跑不了、什么也做不了
     *    （实测被 USB-Serial/JTAG 的 RX 中断刷了 1.2MB 日志）。
     *    所以前几次打日志，之后**把这个 CLIC 中断关掉**，让系统活下来。*/
    if (spammed < 3u) {
        rt_kprintf("!! unhandled interrupt id=%d (mcause & 0x1F) —— 该 CLIC 中断将被关闭\n",
                   vector);
    }
    spammed++;
    if (vector >= 16 && vector < S31_ISR_MAX) {
        S31_REG8(S31_CLIC_IE(vector)) = 0;              /* 关掉，别再进来 */
    }
}

/* 注意：异常分支里**不要**用 rt_kprintf 的常规路径之外的东西，也不要返回 */
void s31_usj_flush(rt_uint32_t max_pumps);      /* bsp/drv_usj.c：阻塞把发送环排空 */

static void s31_exception(rt_uint32_t mcause)
{
    rt_uint32_t mepc, mtval, mstatus;
    __asm__ volatile ("csrr %0, mepc"    : "=r"(mepc));
    __asm__ volatile ("csrr %0, mtval"   : "=r"(mtval));
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));

    rt_kprintf("\n*** S31 EXCEPTION ***\n");
    rt_kprintf("  mcause  = 0x%08x\n", (unsigned)mcause);
    rt_kprintf("  mepc    = 0x%08x\n", (unsigned)mepc);
    rt_kprintf("  mtval   = 0x%08x\n", (unsigned)mtval);
    rt_kprintf("  mstatus = 0x%08x\n", (unsigned)mstatus);
    rt_kprintf("(halt)\n");

    /* 🚨 这几行是塞进发送环的，而排空靠 tick 中断 —— 进了下面那个 for(;;)
     *    中断就不来了。不在这里主动刷一次，主机收到的就是"突然无声"
     *    （2026-09-26 为这个白查一轮：其实异常信息一直都在环里躺着）。*/
    s31_usj_flush(20000u);

    for (;;) {
        /* 停机：继续执行故障指令只会无限刷屏 */
    }
}

/* 覆盖 trap_common.c 的弱符号 */
void rt_hw_interrupt_init(void)
{
    int i;
    for (i = 0; i < S31_ISR_MAX; i++) {
        s31_isr_table[i].handler = s31_default_isr;
        s31_isr_table[i].param   = RT_NULL;
    }
}

rt_isr_handler_t rt_hw_interrupt_install(int vector, rt_isr_handler_t handler,
                                        void *param, const char *name)
{
    rt_isr_handler_t old = RT_NULL;
    (void)name;

    if (vector >= 0 && vector < S31_ISR_MAX && handler != RT_NULL) {
        old = s31_isr_table[vector].handler;
        s31_isr_table[vector].handler = handler;
        s31_isr_table[vector].param   = param;
    }
    return old;
}

void rt_rv32_system_irq_handler(rt_uint32_t mcause)
{
    if (mcause & 0x80000000u) {
        rt_uint32_t id = mcause & 0x1Fu;          /* CLIC ID（我们只用 16~31） */
        s31_isr_table[id].handler((int)id, s31_isr_table[id].param);
    } else {
        s31_exception(mcause);
    }
}
