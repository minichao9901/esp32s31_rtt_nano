/*===========================================================================
 * rtconfig.h -- RT-Thread Nano 风格配置（ESP32-S31 / RISC-V RV32）
 *
 * 与官方 BSP 参考（bsp/core-v-mcu/core-v-cv32e40p/rtconfig.h）的差别：
 *   1. 不定义 RT_USING_DEVICE —— 这是" Nano 模式"的关键：
 *      finsh 的输入直接走 rt_hw_console_getchar()（components/finsh/shell.c:196）
 *      ，不需要设备框架/串口驱动。
 *   2. 不要 DFS / 串口 / PIN / libc 组件，内核 + msh 而已。
 *   3. 保留 RT_USING_USER_MAIN + RT_USING_COMPONENTS_INIT：
 *      main 线程里跑 rt_components_init()，finsh_system_init() 靠
 *      INIT_APP_EXPORT 自动起来（shell.c:1019）。
 *===========================================================================*/

#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* ---- 内核基础 ---- */
#define RT_NAME_MAX                 12
#define RT_CPUS_NR                  1
#define RT_ALIGN_SIZE               8
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX      32
#define RT_TICK_PER_SECOND          1000
#define IDLE_THREAD_STACK_SIZE      512
#define RT_USING_TIMER_SOFT
#define RT_TIMER_THREAD_PRIO        4
#define RT_TIMER_THREAD_STACK_SIZE  1024

/* ---- IPC（msh 要用信号量；其余很便宜，一起开）---- */
#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE

/* ---- 内存：小内存管理 + 当堆用 ---- */
#define RT_USING_SMALL_MEM
#define RT_USING_SMALL_MEM_AS_HEAP
#define RT_USING_HEAP

/* ---- 控制台（无设备框架：rt_kprintf -> rt_hw_console_output）---- */
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE          128

/* ---- 设备框架 + 三个基础外设 ----
 * 打开 RT_USING_DEVICE 之后：rt_kprintf 走 console 设备（bsp/drv_usj_dev.c 注册的
 * 字符设备 "usj"），finsh 也从轮询改成"rx_indicate 唤醒"。
 * SPI/I2C/PIN 都建在设备框架上，所以这一组要一起开。
 * 注意：不开 RT_USING_DM（那是设备树/设备模型那套，用不上）。 */
#define RT_USING_DEVICE
#define RT_USING_PIN
#define RT_USING_SPI
#define RT_USING_QSPI
#define RT_USING_I2C

/* ---- SFUD：走 RT-Thread **官方组件**（2026-09-25 从自写移植层切回来）----
 * 官方那套在 rt-thread 源码树里自带，本工程一行都不用写：
 *   components/drivers/spi/sfud/{inc,src}       SFUD 引擎本体
 *   components/drivers/spi/dev_spi_flash_sfud.c 官方移植层 + `sf` 命令 + **注册块设备**
 * 由 tools/build.ps1 直接编（不拷进 bsp/），bsp/drv_spi_flash.c 里调一次
 * `rt_sfud_flash_probe()` 就带起来了 —— 正是"注册一个 SPI 设备就有 SFUD"的官方姿势。
 * 块设备留着是为了以后挂 DFS（`spi_flash0`，扇区 = 擦除粒度 4KB）。
 * RT_SFUD_USING_SFDP：先走 JEDEC SFDP 自动解析；解析不了再查内置型号表。 */
#define RT_USING_SFUD
#define RT_SFUD_USING_SFDP
#define RT_SFUD_USING_FLASH_INFO_TABLE
/* SFUD 给 flash 用的 SPI 时钟上限。官方默认 50000000，会被分频成 40MHz ——
 * 本板这套杜邦线只验过 20MHz（工作区 spi_flash_sfud 工程实测 20MHz 下 98.7% 线速），
 * 所以先按 20MHz 定住；想试更快改这个数即可。*/
#define RT_SFUD_SPI_MAX_HZ          20000000

/* 打开调试日志：**SFUD 的 SFUD_INFO 就是 rtdbg 的 LOG_I**，而没开 RT_USING_DEBUG 时
 * rtdbg 的 LOG_* 全是空宏 —— 那样"识别到 Winbond 8MB"、"找不到芯片"这些信息
 * 会一声不响地消失（本工程踩过）。开了之后 SFUD 按 DBG_LVL=DBG_INFO 输出。*/
#define RT_USING_DEBUG

#define RT_VER_NUM                  0x50202     /* v5.2.2 */
#define RT_BACKTRACE_LEVEL_MAX_NR   32          /* kservice.c:422 用到（无 guard） */

/* ---- 架构 ---- */
#define ARCH_RISCV
#define ARCH_CPU_32BIT

/* ---- 组件自动初始化 + main 线程 ---- */
#define RT_USING_COMPONENTS_INIT
#define RT_USING_USER_MAIN
#define RT_MAIN_THREAD_STACK_SIZE   3072
#define RT_MAIN_THREAD_PRIORITY     10

/* ---- finsh / msh ---- */
#define RT_USING_MSH
#define RT_USING_FINSH
#define FINSH_USING_MSH
#define FINSH_THREAD_NAME           "tshell"
#define FINSH_THREAD_PRIORITY       20
/* 4096 → 8192（2026-09-25）：`sf` 系列命令把 SFUD 那几层调用都压在 tshell 线程上
 * （sfud_device_init + 页编程的 260 字节写缓冲 + 移植层的 5 条 message 链），
 * 实测开着诊断日志时 4096 只剩 496 字节余量 —— 加一倍换个安心，代价仅 4KB RAM。*/
#define FINSH_THREAD_STACK_SIZE     8192
#define FINSH_USING_HISTORY
#define FINSH_HISTORY_LINES         5
#define FINSH_USING_SYMTAB
#define FINSH_CMD_SIZE              80
#define MSH_USING_BUILT_IN_COMMANDS
#define FINSH_USING_DESCRIPTION
#define FINSH_ARG_MAX               10

/* ---- 本板（BSP）---- */
#define BSP_S31_SYSTICK_HZ          1000        /* SYSTIMER 1ms tick */

#endif /* RT_CONFIG_H__ */
