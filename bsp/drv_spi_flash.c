/*===========================================================================
 * drv_spi_flash.c -- 外接 SPI flash：走 RT-Thread **官方 SFUD 组件**
 *
 * 这一层只有一件事：调官方的 `rt_sfud_flash_probe()`，把外接 flash 挂成块设备。
 * SFUD 引擎本体、官方移植层（wr/lock/unlock 钩子）、`sf` 命令**全都是 rt-thread
 * 源码树自带的**（`components/drivers/spi/`），由 build.ps1 直接编，本工程不拷贝、不改：
 *
 *      components/drivers/spi/sfud/{inc,src}/          SFUD 引擎
 *      components/drivers/spi/dev_spi_flash_sfud.c     官方移植层 + `sf` 命令 + 块设备注册
 *
 * 为什么这里只写十几行（而不是自己写一套移植层）：
 *   · 官方那套就是"注册一个 SPI 设备 → SFUD 就起来了"，没有额外机制；
 *   · 它顺手把 flash 注册成**块设备**（RT_Device_Class_Block），以后要挂 DFS
 *     直接对上：扇区 = 擦除粒度（本模块 4KB），`dfs_mount` 之外不用配别的东西。
 *
 * 接线（drv_spi.c 的默认档，也是本工作区 spi_flash_sfud 工程那套）：
 *   SCK=GPIO43(J2-17)  MOSI=GPIO44(J2-18)  MISO=GPIO45(J2-15)  CS=GPIO46(J2-16)
 *
 * sdkconfig/rtconfig.h 那侧的开关（bsp/rtconfig.h）：
 *   RT_USING_SFUD / RT_SFUD_USING_SFDP / RT_SFUD_USING_FLASH_INFO_TABLE / RT_USING_DEBUG
 * ⚠️ RT_USING_DEBUG 必须开：SFUD 的 SFUD_INFO 就是 rtdbg 的 LOG_I，
 *    没开的话 LOG_* 全是空宏，"识别到什么芯片"这类信息会一声不响。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include "dev_spi_flash_sfud.h"      /* 官方组件头（components/drivers/spi/） */

#define S31_FLASH_SPI_DEV   "flash0"        /* drv_spi.c 挂在 spi2 上的 SPI 设备 */
#define S31_FLASH_BLK_DEV   "spi_flash0"    /* 识别成功后注册出来的块设备名 */

/* 组件级（.rti_fn.4）：spi2/flash0 是设备级（.rti_fn.3）建的，级别上保证跑在它之后 ——
 * 别改成 INIT_DEVICE_EXPORT，同级之间的先后取决于链接顺序，不可靠。*/
static int s31_spi_flash_init(void)
{
    rt_spi_flash_device_t dev;

    dev = rt_sfud_flash_probe(S31_FLASH_BLK_DEV, S31_FLASH_SPI_DEV);
    if (dev == RT_NULL) {
        rt_kprintf("[sfud] 没识别到外接 flash —— 先敲 `spi_id` 看 JEDEC ID"
                   "（EF 40 17 = W25Q64 正常）\n");
        return -RT_ERROR;               /* 失败不中止启动：其余功能照常 */
    }

    rt_kprintf("[sfud] %s 挂成块设备 \"%s\"：%u 扇区 × %u 字节 = %u KB\n",
               S31_FLASH_SPI_DEV, S31_FLASH_BLK_DEV,
               (unsigned)dev->geometry.sector_count,
               (unsigned)dev->geometry.bytes_per_sector,
               (unsigned)((dev->geometry.sector_count / 1024u) *
                          (dev->geometry.bytes_per_sector / 1024u) * 1024u));
    rt_kprintf("[sfud] 命令：sf probe %s / sf read|write|erase|status；"
               "块设备用 `list device` 看（RT-Thread 5.x 把 list_device 并进了 list）\n",
               S31_FLASH_SPI_DEV);
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(s31_spi_flash_init);
