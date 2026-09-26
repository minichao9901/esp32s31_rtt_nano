/*===========================================================================
 * sfud_cfg.h -- SFUD 的配置头（**本工程自己写的**，替换 RT-Thread 那份）
 *
 * 上游那份（rt-thread/components/drivers/spi/sfud/inc/sfud_cfg.h）绑了
 * RT-Thread 的组件开关（RT_SFUD_USING_SFDP 等）和 rtdbg 日志框架，
 * 本 port 不走 Kconfig，所以这里直接写死，并把日志接到 rt_kprintf。
 *
 * 与上游的差别（3 处）：
 *   ① 功能开关直接定义（SFUD_USING_SFDP / FLASH_INFO_TABLE），不再看 RT_* 宏；
 *   ② SFUD_INFO 直接进 rt_kprintf（**自己补换行**：SFUD 的调用点全都不带 '\n'，
 *      少了这句十几条信息会挤成一整行 —— 本工作区 spi_flash_sfud 工程踩过）；
 *   ③ **不开 SFUD_USING_QSPI**（理由见下）。
 *
 * 🚨 为什么不开 QSPI：
 *   `sfud_qspi_fast_read_enable()` 只是把读命令换成 0xEB（数据相位 4 线），
 *   而**四线读必须额外接 IO2/IO3（WP/HD）两根线**，本板的接线（J2-15/16/17/18
 *   = 45/46/43/44）里没有它们。更关键的是：那条路要求 SPI 主机在数据相位
 *   能把 4 根线的输出使能交给从机，本 port 的 `drv_spi.c` 在 GPIO matrix 下做不到
 *   （IDF 的 `check_iomux_pins_quad()` 也是这个结论，见 drv_spi.c 文件头）。
 *   → 单线读本来就稳（实测能到线速的九成），为了一个"跑不通还会读出垃圾"的
 *     模式去多背一张表和一条代码路径不值得。哪天真接了 6 根线并把接线切到
 *     专用 IO_MUX，再把这两个宏打开即可（sfud.c 里那条路径是现成的）。
 *
 * 设备表：**静态 1 个设备**（上游允许在这里写死，但那样每次 `sf probe` 都得重编；
 *   本 port 在 `s31_sfud_probe()` 里运行时填 name/索引，所以这里给个空壳。
 *   `{{0}}` 这个写法照抄上游 —— 它等价于"一个全 0 的 sfud_flash"。*/
#ifndef _SFUD_CFG_H_
#define _SFUD_CFG_H_

#include <rtthread.h>

/* 调试：打开后 SFUD 会打每条命令级的日志（很吵，仅排错时用）*/
/* #define SFUD_DEBUG_MODE */

/* 打开 JEDEC SFDP 解析（0x5A）—— W25Q64FV 支持；解析失败会自动退回下面的型号表 */
#define SFUD_USING_SFDP

/* 打开内置型号表（JEDEC ID → 容量/擦除粒度），SFDP 不可用时靠它兜底 */
#define SFUD_USING_FLASH_INFO_TABLE

/* 见文件头"为什么不开 QSPI" */
/* #define SFUD_USING_QSPI */

/* 静态设备表：1 个设备，参数由 s31_sfud_probe() 运行时填 */
#define SFUD_FLASH_DEVICE_TABLE {{0}}

/* 日志：直接进控制台，**补 '\n'** */
#ifdef SFUD_DEBUG_MODE
/* 调试模式下 SFUD_DEBUG(...) 会调 sfud_log_debug()（见 sfud_def.h:46），
 * 那个要在 port 里实现 —— 本工程用 rt_kprintf 兜住它。*/
void sfud_log_debug(const char *file, const long line, const char *format, ...);
#define SFUD_INFO(...)  rt_kprintf(__VA_ARGS__), rt_kprintf("\n")
#else
#define SFUD_INFO(...)  rt_kprintf(__VA_ARGS__), rt_kprintf("\n")
#endif

#endif /* _SFUD_CFG_H_ */
