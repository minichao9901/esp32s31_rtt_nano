/*===========================================================================
 * rtconfig.h -- RT-Thread Nano 风格配置（ESP32-S31 / RISC-V RV32）
 *
 * 与官方 BSP 参考（bsp/core-v-mcu/core-v-cv32e40p/rtconfig.h）的差别：
 *   1. 不定义 RT_USING_DEVICE —— 这是" Nano 模式"的关键：
 *      finsh 的输入直接走 rt_hw_console_getchar()（components/finsh/shell.c:196）
 *      ，不需要设备框架/串口驱动。
 *      （2026-09-25 起 RT_USING_DEVICE 还是开了：SPI/I2C/PIN/块设备都要它。）
 *   2. 串口/PIN libc 组件按需开；**DFS + elmfat 已经开了**（见下面的段落）。
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

/* ---- DFS + elm-chan FatFs：**RT-Thread 官方组件**（2026-09-26 加）----
 * 源码全在 rt-thread 源码树里，由 build.ps1 直接编，本工程一行都不改：
 *   components/dfs/dfs_v1/src/                   VFS 本体（v1；v2 是 RT-Thread Smart 那套）
 *   components/dfs/dfs_v1/filesystems/elmfat/    FatFs 本体 + 官方 diskio 移植层（dfs_elm.c）
 *   components/dfs/dfs_v1/filesystems/devfs/     /dev 下的设备节点
 *   components/finsh/msh_file.c                  ls/cat/mkdir/rm/cp/mv/cd/pwd/mkfs/mount/df
 *
 * 🚨 DFS_USING_POSIX 必须开：上面那一整排 msh 命令被 RT-Thread 5.x 挪进了
 *    msh_file.c，而它的守卫是 `#if defined(RT_USING_FINSH) && defined(DFS_USING_POSIX)`
 *    —— 不开的话文件系统能挂上，但**一条文件命令都没有**（本工程踩过）。
 *
 * 🚨 RT_DFS_ELM_MAX_SECTOR_SIZE 必须 ≥ 4096：外接 SPI NOR 的"扇区"就是它的
 *    擦除粒度 4KB（官方 SFUD 移植层把 geometry.bytes_per_sector 定成 erase_gran），
 *    而 dfs_elm.c 挂载时会检查 `bytes_per_sector > FF_MAX_SS` 就直接报错拒挂。
 *    这一项在 FatFs 里是"可变扇区"配置（FF_MIN_SS=512 / FF_MAX_SS=4096）：
 *    FatFs 挂载时**问设备要 GET_SECTOR_SIZE**，所以板载盘（512B）和外接盘（4KB）
 *    可以同时存在，各自的 FAT 卷也各按自己的扇区大小建。 */
#define RT_USING_DFS
#define RT_USING_DFS_V1
#define DFS_USING_POSIX
#define DFS_USING_WORKDIR
#define DFS_FD_MAX                  8
#define DFS_FILESYSTEMS_MAX         4
#define DFS_FILESYSTEM_TYPES_MAX    4

#define RT_USING_DFS_ELMFAT
#define RT_DFS_ELM_CODE_PAGE        936     /* 简体中文（GBK）：Windows 拷进来的中文名可读 */
#define RT_DFS_ELM_WORD_ACCESS
#define RT_DFS_ELM_USE_LFN          3       /* 长文件名；工作缓冲走堆（ff_memalloc）*/
#define RT_DFS_ELM_MAX_LFN          255
#define RT_DFS_ELM_DRIVES           2       /* 两个卷：外接 spi_flash0 + 板载 onboard0 */
#define RT_DFS_ELM_MAX_SECTOR_SIZE  4096    /* 见上面的说明，别改回 512 */
#define RT_DFS_ELM_REENTRANT
#define RT_DFS_ELM_MUTEX_TIMEOUT    3000

#define RT_USING_DFS_DEVFS                  /* /dev 下的设备节点（有设备框架才能开）*/

/* ---- CherryUSB（RT-Thread 源码树自带；2026-09-26 加）----
 * 这些宏对应官方 components/drivers/usb/cherryusb/Kconfig.rtt 里的选项，
 * 作用有两个：① 告诉我们自己该编哪些文件（见 tools/build.ps1 ⑦）；
 * ② 官方 demo 模板里用 `#if defined(RT_CHERRYUSB_DEVICE_TEMPLATE_MSC_BLKDEV)`
 *    选"盘 = RT-Thread 块设备"那条分支 —— 少了它就会走成内存盘。
 * 真正的 CherryUSB 参数（缓冲大小、盘名字、日志开关）在 bsp/usb_config.h。*/
#define RT_USING_CHERRYUSB
#define RT_CHERRYUSB_DEVICE
#define RT_CHERRYUSB_DEVICE_SPEED_HS
#define RT_CHERRYUSB_DEVICE_DWC2_CUSTOM     /* S31 的胶水在本工程 bsp/drv_usb_msc.c */
#define RT_CHERRYUSB_DEVICE_MSC
#define RT_CHERRYUSB_DEVICE_TEMPLATE_MSC_BLKDEV

/* ---- audio 设备框架（bsp/drv_audio_pwm.c 用它注册 sound0；app/wav_player.c 用它放音）
 * 框架本体是官方的 components/drivers/audio/dev_audio.c：
 *   应用 write → 内存池/队列切块 → ops->start(REPLAY) → 硬件播完一块调
 *   rt_audio_tx_complete() → 框架填下一块（乒乓）。驱动侧只需要 buffer_info +
 *   采样时钟 + 每块完成时回调，见 bsp/drv_audio_pwm.c 顶部那张图。*/
#define RT_USING_AUDIO
/* 🚨 audio 框架的 replay 缓冲是**内存池**（rt_mp_create/alloc/free）——
 *    开了 RT_USING_AUDIO 就必须一起开 RT_USING_MEMPOOL，
 *    否则 dev_audio.c 里那几个 rt_mp_* 全是隐式声明（编不过）。*/
#define RT_USING_MEMPOOL
/* device IPC：数据队列/完成量/环形缓冲（都在 components/drivers/ipc/ 下）。
 * audio 框架的 replay 队列用 rt_data_queue + rt_completion，音频管道用 rt_ringbuffer，
 * 所以这一组必须开（rtconfig 里叫 device IPC，容易以为是"管道"专用，其实不止）。*/
#define RT_USING_DEVICE_IPC
#define RT_AUDIO_REPLAY_MP_BLOCK_SIZE   4096    /* 队列里每块 4KB（约 256ms @16k/8bit）*/
#define RT_AUDIO_REPLAY_MP_BLOCK_COUNT  4       /* 池里 4 块 → 最多缓冲 ~1 秒 */
#define RT_AUDIO_RECORD_PIPE_SIZE       2048    /* 不放音（只用放音），留着默认值 */

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
