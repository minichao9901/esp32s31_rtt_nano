/*===========================================================================
 * drv_usj_dev.c -- 把 USB-Serial/JTAG 注册成 RT-Thread 字符设备（控制台）
 *
 * 为什么需要它：一旦打开 RT_USING_DEVICE，finsh 就不再走 rt_hw_console_getchar()，
 * 而是（components/finsh/shell.c:157-199、219-255）：
 *   1. 在 finsh_thread_entry 里发现 shell->device == NULL，
 *      就去拿 rt_console_get_device() 的名字（shell.c:522-527）
 *   2. rt_device_open(dev, RDWR|INT_RX|STREAM) 打开它
 *   3. 用 rt_device_read(dev, -1, &ch, 1) 取字符；
 *      读不到就睡在 shell->rx_sem 上，**由设备的 rx_indicate 回调唤醒**
 *      （shell.c:202-210 的 finsh_rx_ind）
 * 所以这个设备必须：能 read、能 write、并且在收到主机数据时调 rx_indicate。
 * 收数据用 USJ 中断（CLIC ID 17，源 2 = ETS_USB_SERIAL_JTAG）——比原来的轮询
 * 还顺带把输入延迟从 10ms 降到 ~0。
 *
 * 发送复用 drv_usj.c 的 2KB 环形缓冲（s31_usj_put_bytes + s31_usj_tx_pump）。
 *===========================================================================*/

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "s31_regs.h"

#define USJ_NAME            "usj"
#define USJ_RX_RING_SIZE    512u
#define USJ_RX_RING_MASK    (USJ_RX_RING_SIZE - 1u)

/* RX 中断用 CLIC ID 17（外部中断第 2 个口；16 已被 systick 占了） */
#define USJ_RX_CLIC_ID      (S31_CLIC_EXT_OFFSET + 1)

static volatile rt_uint8_t  s_rx_ring[USJ_RX_RING_SIZE];
static volatile rt_uint32_t s_rx_head;      /* 写指针（ISR） */
static volatile rt_uint32_t s_rx_tail;      /* 读指针（线程） */

static struct rt_device s_usj_dev;

void s31_usj_put_bytes(const char *str, rt_size_t len);
void s31_usj_tx_pump(void);

/* ---- 设备接口 ---------------------------------------------------------- */

static rt_err_t usj_init(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_err_t usj_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;

    /* 清掉可能在 ROM/bootloader 阶段挂起的标志，再开"收到数据"中断 */
    S31_REG32(S31_USJ_INT_CLR)  = S31_USJ_INT_RX;
    S31_REG32(S31_USJ_INT_ENA) |= S31_USJ_INT_RX;
    return RT_EOK;
}

static rt_err_t usj_close(rt_device_t dev)
{
    (void)dev;
    S31_REG32(S31_USJ_INT_ENA) &= ~S31_USJ_INT_RX;
    return RT_EOK;
}

static rt_ssize_t usj_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_size_t n = 0;
    rt_base_t level;

    (void)dev;
    (void)pos;

    level = rt_hw_interrupt_disable();          /* 与 RX 中断互斥（单核，关中断就够） */
    while (n < size && s_rx_tail != s_rx_head) {
        ((rt_uint8_t *)buffer)[n++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) & USJ_RX_RING_MASK;
    }
    rt_hw_interrupt_enable(level);

    return (rt_ssize_t)n;
}

static rt_ssize_t usj_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;

    s31_usj_put_bytes((const char *)buffer, size);
    return (rt_ssize_t)size;
}

/* ---- RX 中断：搬空 FIFO → 环形缓冲 → 唤醒 finsh ------------------------- */

static void s31_usj_rx_isr(int irq, void *param)
{
    (void)irq;
    (void)param;

    while (S31_REG32(S31_USJ_EP1_CONF) & S31_USJ_OUT_EP_DATA_AVAIL) {
        rt_uint8_t  c    = (rt_uint8_t)(S31_REG32(S31_USJ_EP1) & 0xFFu);
        rt_uint32_t next = (s_rx_head + 1u) & USJ_RX_RING_MASK;
        if (next != s_rx_tail) {                /* 满则丢，绝不阻塞 */
            s_rx_ring[s_rx_head] = c;
            s_rx_head = next;
        }
    }

    S31_REG32(S31_USJ_INT_CLR) = S31_USJ_INT_RX;

    if (s_usj_dev.rx_indicate != RT_NULL) {
        s_usj_dev.rx_indicate(&s_usj_dev, 1);   /* 唤醒 finsh（finsh_rx_ind 释放信号量）*/
    }
}

/*===========================================================================
 * 从**别的**输入通道往控制台 RX 环里塞字节（目前只有 SEGGER RTT 用，见
 * bsp/drv_rtt.c）。这样 RTT 的输入和 USB-CDC 的输入走**同一条**交付路径：
 * 同一个环、同一个 rx_indicate → finsh 不用知道字符是从哪来的。
 * 关中断写环，和 RX 中断互斥（和 ISR 里那段是同一个套路）。
 *===========================================================================*/
void s31_usj_rx_inject(const char *buf, rt_size_t len)
{
    rt_size_t i;
    rt_base_t level;

    if ((buf == RT_NULL) || (len == 0u)) {
        return;
    }

    level = rt_hw_interrupt_disable();
    for (i = 0; i < len; i++) {
        rt_uint32_t next = (s_rx_head + 1u) & USJ_RX_RING_MASK;
        if (next == s_rx_tail) {
            break;                              /* 满则丢，绝不阻塞 */
        }
        s_rx_ring[s_rx_head] = (rt_uint8_t)buf[i];
        s_rx_head = next;
    }
    rt_hw_interrupt_enable(level);

    if (s_usj_dev.rx_indicate != RT_NULL) {
        s_usj_dev.rx_indicate(&s_usj_dev, 1);
    }
}

/* ---- 注册（device 级：早于 finsh 的 app 级，晚于调度器起来）------------- */

static int s31_usj_dev_register(void)
{
    /* 🚨 顺序很重要（踩过：先开 CLIC IE 再装 handler，遇到"开机时 RX 已有数据"
     *    就会以 NULL handler 进中断 → 没人清 USJ 的中断标志 → 电平触发**死循环刷屏**）：
     *    ① 先关外设中断 + 清挂起 ② 装 handler ③ 配路由/优先级 ④ 最后才使能 CLIC。*/
    S31_REG32(S31_USJ_INT_ENA) &= ~S31_USJ_INT_RX;
    S31_REG32(S31_USJ_INT_CLR)  = S31_USJ_INT_RX;

    rt_hw_interrupt_install(USJ_RX_CLIC_ID, s31_usj_rx_isr, RT_NULL, "usj-rx");

    /* 路由矩阵：源 2（USB_SERIAL_JTAG）→ CLIC ID 17；电平触发、优先级 1、使能 */
    S31_REG32(S31_INTR0_BASE + 4 * S31_ETS_USB_SERIAL_JTAG) = USJ_RX_CLIC_ID;
    S31_REG8(S31_CLIC_IP(USJ_RX_CLIC_ID))   = 0;
    S31_REG8(S31_CLIC_ATTR(USJ_RX_CLIC_ID)) = 0;
    S31_REG8(S31_CLIC_CTL(USJ_RX_CLIC_ID))  = S31_CLIC_CTL_PRIO(1);
    S31_REG8(S31_CLIC_IE(USJ_RX_CLIC_ID))   = 1;

    s_usj_dev.type        = RT_Device_Class_Char;
    s_usj_dev.rx_indicate = RT_NULL;
    s_usj_dev.tx_complete = RT_NULL;
    s_usj_dev.init        = usj_init;
    s_usj_dev.open        = usj_open;
    s_usj_dev.close       = usj_close;
    s_usj_dev.read        = usj_read;
    s_usj_dev.write       = usj_write;
    s_usj_dev.control     = RT_NULL;
    s_usj_dev.user_data   = RT_NULL;

    rt_device_register(&s_usj_dev, USJ_NAME,
                       RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM);

    /* 设为控制台：rt_kprintf 从此走设备（kservice.c:314-332），
     * finsh 也会自动认领它（shell.c:522-527）。 */
    rt_console_set_device(USJ_NAME);
    return 0;
}
INIT_DEVICE_EXPORT(s31_usj_dev_register);
