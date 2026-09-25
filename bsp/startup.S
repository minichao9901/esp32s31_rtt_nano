/*===========================================================================
 * startup.S -- ESP32-S31 启动（bootROM 直接跳到这里，没有 IDF bootloader）
 *
 * ROM 已经把镜像各段按 VMA 搬进 RAM，这里只做裸机必须自己做的事：
 *   1. 非 0 号 hart 停住
 *   2. 关看门狗（RTC_WDT + TIMG0/1 的 MWDT；实测 MWDT 会在 1 秒后复位芯片）
 *   3. 设启动栈
 *   4. 关全局中断、mscratch = 中断栈顶（SW_handler 靠它换栈）
 *   5. mtvec = SW_handler | 3（CLIC 模式）
 *   6. 清 .bss
 *   7. 进 RT-Thread：entry()（src/components.c:160）
 *===========================================================================*/

.section .boot, "ax"
.globl _start
.type _start, @function
.align 2

.set RTC_WDT_BASE,     0x20801000
.set RTC_WDT_CONFIG0,  0x00
.set RTC_WDT_WPROTECT, 0x18
.set RTC_WDT_SWD_CONF, 0x1c            /* SWD_CONFIG：超级看门狗 */
.set RTC_WDT_SWD_WPROT, 0x20
.set TIMG0_BASE,       0x20580000
.set TIMG1_BASE,       0x20581000
.set TIMG_WDTCONFIG0,  0x48
.set TIMG_WDTWPROTECT, 0x64
.set WDT_WKEY,         0x50D83AA1

_start:
        /* 只有 core0 干活 */
        csrr    t0, mhartid
        bnez    t0, .Lpark

        /* ---- 关看门狗 ---- */
        li      t1, WDT_WKEY
        li      t0, RTC_WDT_BASE
        sw      t1, RTC_WDT_WPROTECT(t0)
        sw      zero, RTC_WDT_CONFIG0(t0)
        sw      zero, RTC_WDT_WPROTECT(t0)

        li      t0, TIMG0_BASE
        sw      t1, TIMG_WDTWPROTECT(t0)
        sw      zero, TIMG_WDTCONFIG0(t0)
        sw      zero, TIMG_WDTWPROTECT(t0)
        li      t0, TIMG1_BASE
        sw      t1, TIMG_WDTWPROTECT(t0)
        sw      zero, TIMG_WDTCONFIG0(t0)
        sw      zero, TIMG_WDTWPROTECT(t0)

        /* ---- 超级看门狗（SWD）：**必须喂**，否则 ~3.4 秒整片复位一次 ----
         * 🚨 2026-09-23 实测：复位原因寄存器报 0x12 = 超级看门狗。
         *    它盯的是 RTC_WDT 的喂狗信号，而我们把 RTC_WDT 关了 → 没人喂 → 定期复位。
         *    这解释了"板子每 3 秒左右重启一次、串口只能看到开机那几行"的现象
         *    （还一度被 ROM 里那个**没清的旧值** rst:0x17 误导成"USB 请求的复位"）。
         *    做法照 IDF `bootloader_esp32c6.c: bootloader_super_wdt_auto_feed()`：
         *    解锁 → 打开 SWD_AUTO_FEED_EN(bit18) → 上锁。*/
        li      t0, RTC_WDT_BASE
        li      t1, WDT_WKEY
        sw      t1, RTC_WDT_SWD_WPROT(t0)
        lw      t2, RTC_WDT_SWD_CONF(t0)
        li      t3, (1 << 18)                  /* SWD_AUTO_FEED_EN */
        or      t2, t2, t3
        sw      t2, RTC_WDT_SWD_CONF(t0)
        sw      zero, RTC_WDT_SWD_WPROT(t0)

        /* ---- 启动栈 ---- */
        la      sp, __stack_top

        /* ---- gp（小数据指针）：没有 crt0，必须自己装 ----
         * GCC 默认把 ≤8 字节的静态变量放进 .sdata/.sbss 并用 gp 相对寻址，
         * gp 没设好就会去随机地址读写。.option norelax 是标准做法：
         * 否则 la 会被展开成 gp 相对的 GOT 访问，鸡生蛋。 */
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop

        /* ---- 关中断 + 中断栈 + 陷阱入口 ---- */
        csrci   mstatus, 0x8                    /* MIE = 0 */
        /* 打开 FPU（mstatus.FS=3）：-mabi=ilp32f 下陷阱处理要 fsw f0-f31，
         * FS=0 时那几条会二次陷阱。线程第一条由 rt_hw_stack_init 设 0x7880。 */
        li      t0, 0x6000
        csrs    mstatus, t0
        la      t0, __rt_rvstack
        csrw    mscratch, t0                    /* SW_handler 进来先换成这个栈 */
        la      t0, s31_clic_entry              /* 64 字节对齐的跳板，见 trap_gcc.S */
        ori     t0, t0, 3                       /* MTVEC_MODE_CSR = 3 (CLIC) */
        csrw    mtvec, t0

        /* ---- 清 .bss ---- */
        la      t0, __bss_start
        la      t1, __bss_end
        bgeu    t0, t1, 2f
1:      sw      zero, 0(t0)
        addi    t0, t0, 4
        bltu    t0, t1, 1b
2:
        call    entry                           /* RT-Thread 启动流程 */

.Lpark:
        wfi
        j       .Lpark

.size _start, .-_start
