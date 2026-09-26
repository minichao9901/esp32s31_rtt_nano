/*
 * usb_config.h -- CherryUSB 配置（ESP32-S31 / RT-Thread）
 *
 * 这是 CherryUSB 约定的配置入口：`usbd_core.h` 会 `#include "usb_config.h"`，
 * 所以本文件必须出现在 include 路径里（build.ps1 已加 `-I$Bsp`）。
 *
 * 与"裸机版"（projects/boot_msc_s31/bsp/usb_config.h）的差别只有两处：
 *   ① printf 走 RT-Thread 的 rt_kprintf（那边是裸机 kprintf）；
 *   ② MSC 的收发处理搬进**线程**（CONFIG_USBDEV_MSC_THREAD）—— 官方 blkdev
 *      模板里对这一点有 #error 硬要求：块设备读写要几毫秒，放中断里会把
 *      USB 时序拖垮（和本工作区 §4.7 那条"ISR 里别做重活"是同一个道理）。
 *
 * 引脚/口：USB-HS（Type-A，S31 USB2.0 OTG-HS，DWC2 @0x20300000，UTMI PHY）。
 */
#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#include <stdint.h>

/* ================ USB common ================ */
#include <rtthread.h>

/* 日志闸门：CherryUSB 的 USB_LOG_ERR 在任何等级下都开着，而且有几处调用点在
 * **USB 中断**里（usbd_core.c 的 usbd_print_setup 会把整个 SETUP 包 dump 出来）。
 * 中断里同步写控制台会形成"ISR 变慢 → 主机超时 → 更多错误 → 打印更多"的正反馈，
 * 而且会把 USB 事务间隔从 ~0.3ms 拉到几 ms（本工作区 §4.7 实测过）。
 * 所以：**中断上下文直接丢弃**，线程里照常输出。 */
extern volatile int g_usbd_in_isr;
#define CONFIG_USB_PRINTF(...)                                  \
    do {                                                        \
        if (!g_usbd_in_isr) {                                   \
            rt_kprintf(__VA_ARGS__);                            \
        }                                                       \
    } while (0)

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* 数据对齐（DMA/cache 用；我们走 FIFO 模式，32 足够） */
#ifndef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE 32
#endif

/* FIFO（从）模式下 DWC2 完全不碰内存 → 没有 cache 一致性问题，
   usb_dcache_* 在没定义 CONFIG_USB_DCACHE_ENABLE 时是空宏。 */
#define USB_NOCACHE_RAM_SECTION

/* ================= USB Device ================= */
#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

#define CONFIG_USBDEV_MAX_BUS 1

/* ================= MSC class ================= */
#ifndef CONFIG_USBDEV_MSC_MAX_LUN
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#endif

/* 一次 SCSI 传输的最大数据量（类里的 bounce buffer）。
 * Windows 的 WRITE(10) 常见 64KB 一包 —— 这块缓冲从 RT-Thread 堆里出，
 * 本工程堆只有 ~170KB，所以**取 16KB**：够用（类内部会把大传输拆成多次），
 * 又不会把堆吃掉一大块。真的遇到"大文件写入慢"再往上调。*/
#ifndef CONFIG_USBDEV_MSC_MAX_BUFSIZE
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE (16 * 1024)
#endif

/* 把 MSC 的收发放到线程里（官方 blkdev 模板硬要求）*/
#define CONFIG_USBDEV_MSC_THREAD
#ifndef CONFIG_USBDEV_MSC_STACKSIZE
#define CONFIG_USBDEV_MSC_STACKSIZE 2048
#endif
#ifndef CONFIG_USBDEV_MSC_PRIO
#define CONFIG_USBDEV_MSC_PRIO 12
#endif

/* 这块盘背后是哪个块设备（本工程 = 板载 flash 划出来的 8MB）*/
#ifndef CONFIG_USBDEV_MSC_BLOCK_DEV_NAME
#define CONFIG_USBDEV_MSC_BLOCK_DEV_NAME "onboard0"
#endif

/* SCSI INQUIRY 里报的厂商/产品/版本（资源管理器 → 设备属性里能看到）*/
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING "Espressif"
#define CONFIG_USBDEV_MSC_PRODUCT_STRING      "S31 Onboard Flash"
#define CONFIG_USBDEV_MSC_VERSION_STRING      "1.00"

/* ================= USB Device Port ================= */
#define CONFIG_USB_DWC2_PORT HS_PORT
#define CONFIG_USB_HS

#define ESP_USB_HS0_BASE 0x20300000UL      /* USB-OTG HS (DWC2.0) */
#define USB_BASE         ESP_USB_HS0_BASE

#endif /* USB_CONFIG_H */
