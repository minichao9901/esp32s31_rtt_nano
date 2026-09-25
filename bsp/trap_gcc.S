/*===========================================================================
 * trap_gcc.S -- CLIC 陷阱入口跳板 + 非向量中断统一入口
 *              （RT-Thread RV32 移植指南"方式一"）
 *
 * 【为什么需要跳板】
 * CLIC 模式下 mtvec 的基址字段是 mtvec[31:6]，也就是**陷阱入口必须 64 字节对齐**。
 * 理论上直接把 mtvec 指向 libcpu 的 SW_handler 就行，但实测不行：
 * interrupt_gcc.S 的 .text.entry 段开头有 2 字节 nop 填充（RISC-V 汇编器给
 * .align 放松预留的），SW_handler 落在段内偏移 0x2 上 —— 段对齐到 64 也没用。
 * 所以这里放一个自己的、64 字节对齐的跳板，用 j（不碰任何寄存器）跳过去，
 * 这样既不用改上游源码，也不用担心偏移。
 *
 * 【rt_hw_do_after_save_above】
 * SW_handler（libcpu/risc-v/common/interrupt_gcc.S）保存完现场后会调本函数，
 * 此时 sp 已在中断栈上，mscratch 里是线程栈的现场指针。
 *
 * 与官方参考实现（bsp/core-v-mcu/.../core-v-mcu/source/trap_gcc.S）的差别只有一处：
 * **额外保存/恢复 mcause**。原因（CLIC 专有，IDF riscv/vectors.S:546-563 注释原文）：
 *   "In case the target uses the CLIC, it is mandatory to restore mcause register
 *    since it contains the former CPU priority. When executing mret, the hardware
 *    will restore the former threshold, from mcause to mintstatus CSR"
 * 本 port 在陷阱里关着 MIE（SW_handler 开头 csrci），不会有嵌套覆盖 mcause，
 * 所以这一步严格来说是"防御性"的 —— 但只花 4 条指令，留着踏实。
 *===========================================================================*/

#include "cpuport.h"

    /* ---- CLIC 陷阱入口跳板：必须 64 字节对齐，且不许碰任何寄存器 ---- */
    .section .clic_entry, "ax"
    .align  6
    .global s31_clic_entry
    .type   s31_clic_entry, @function
s31_clic_entry:
    j       SW_handler
    .size   s31_clic_entry, . - s31_clic_entry

    /* ---- 统一陷阱分发（方式一）---- */
    .section .text
    .globl rt_hw_do_after_save_above
    .type rt_hw_do_after_save_above,@function
rt_hw_do_after_save_above:
    addi  sp, sp,  -8
    STORE ra,  0 * REGBYTES(sp)
    csrr  t0, mcause
    STORE t0,  1 * REGBYTES(sp)

    csrr  a0, mcause
    csrr  a1, mepc
    mv    a2, sp
    call  rt_rv32_system_irq_handler

    LOAD  t0,  1 * REGBYTES(sp)
    csrw  mcause, t0
    LOAD  ra,  0 * REGBYTES(sp)
    addi  sp, sp,  8
    ret
