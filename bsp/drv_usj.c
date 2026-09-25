/*===========================================================================
 * drv_usj.c -- msh 控制台：USB-Serial/JTAG（板上 USB-DBG 口 = COM43）
 *
 * 为什么不用 UART0：本板 UART0（GPIO58/59）没接线，USB-DBG 是唯一接着 PC 的口。
 * 这个外设是"硬件固定功能"的 USB 设备（CDC+JTAG 由硬件实现），
 * 我们只跟两个寄存器打交道：
 *   EP1      (+0x00)  写 = 往主机发一个字节；读 = 收主机来的一个字节
 *   EP1_CONF (+0x04)  bit0 wr_done（把整包交给主机）
 *                     bit1 serial_in_ep_data_free（TX FIFO 有空位）
 *                     bit2 serial_out_ep_data_avail（RX FIFO 有数据）
 * 协议来自 IDF hal/usb_serial_jtag_ll.h:116-187。
 *
 * 【为什么要 2KB 发送环形缓冲】
 * TX FIFO 只有 64 字节，而"打开串口就会复位芯片"（USB-JTAG 的 DTR/RTS 就是复位线），
 * 复位后固件开机打印时**主机还没来得及读** → 直接写 FIFO 的话前面的字符全丢，
 * 开机横幅被啃掉半截（实测）。改成：rt_kprintf 先塞环形缓冲，
 * 由 (a) 本次输出结束 (b) 每 1ms 的 tick 中断 两个时机往 FIFO 里灌。
 * 效果：2KB 以内的开机输出一条不丢，主机没读也不阻塞。
 *===========================================================================*/

#include <rtthread.h>
#include "s31_regs.h"

#define USJ_TX_RING_SIZE   2048u                    /* 必须是 2 的幂 */
#define USJ_TX_RING_MASK   (USJ_TX_RING_SIZE - 1u)

static volatile rt_uint8_t  s_tx_ring[USJ_TX_RING_SIZE];
static volatile rt_uint32_t s_tx_head;              /* 写指针（生产者） */
static volatile rt_uint32_t s_tx_tail;              /* 读指针（消费者） */

/* 进 trap 时硬件清 mstatus.MIE → 用这一位判断"我是不是在中断里"。 */
static int in_isr(void)
{
    rt_uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    return (mstatus & 0x8u) == 0;
}

void s31_usj_hw_init(void)
{
    /* 外设时钟/PHY 由 bootROM 配好（M0 实测 ROM 的 printf 就是从这个 FIFO 出去的），
     * 这里只把 PHY 焊盘使能再确认一遍（幂等）。 */
    S31_REG32(S31_USJ_CONF0) |= S31_USJ_CONF0_PAD_ENABLE;
}

/* ---- 环形缓冲的生产者侧 ------------------------------------------------- */
static void usj_ring_put(char c)
{
    int isr = in_isr();
    rt_base_t level = 0;

    if (!isr) {
        level = rt_hw_interrupt_disable();          /* 与 tick 中断里的消费者互斥 */
    }

    rt_uint32_t head = s_tx_head;
    rt_uint32_t next = (head + 1u) & USJ_TX_RING_MASK;
    if (next != s_tx_tail) {                        /* 满则丢字符，绝不阻塞 */
        s_tx_ring[head] = (rt_uint8_t)c;
        s_tx_head = next;
    }

    if (!isr) {
        rt_hw_interrupt_enable(level);
    }
}

/* ---- 消费者：尽量把缓冲灌进 TX FIFO（线程/中断里都能调，不阻塞）--------- */
/* 🚨 实测结论（2026-09-23）：`wr_done` **必须每次调用都写**，哪怕一个字节都没写。
 * 为什么：IDF `usb_serial_jtag_ll.h:173-180` 写得很清楚 —— **装满 64 字节的 FIFO 会被
 * 硬件自动提交，而主机把这个整包当成"未结束的 USB 事务"，要再补一个零长度包
 * （也就是再写一次 wr_done）才会真正交给 CDC 读端**。我们的发送环就是按 64 字节
 * 一块往外灌的，一旦某次正好是整包，接下来环空了、没人再写 wr_done →
 * 主机那边的读请求永远完不成 → 串口一个字节都收不到，几秒后 USB 口直接掉线
 * （现象：`ClearCommError ... 设备不识别此命令`，主机反复重开串口 = "板子反复复位"）。
 * 之前"改成只在写了字节后才置"是**自作聪明的优化**，反而把控制台搞死了。*/
void s31_usj_tx_pump(void)
{
    while (s_tx_tail != s_tx_head) {
        if (!(S31_REG32(S31_USJ_EP1_CONF) & S31_USJ_IN_EP_DATA_FREE)) {
            break;                                   /* FIFO 满，下次再灌 */
        }
        S31_REG32(S31_USJ_EP1) = s_tx_ring[s_tx_tail];
        s_tx_tail = (s_tx_tail + 1u) & USJ_TX_RING_MASK;
    }
    S31_REG32(S31_USJ_EP1_CONF) = S31_USJ_EP1_WR_DONE;
}

/* ---- RT-Thread 控制台挂钩 ----
 * ⚠️ 两种编译配置都要能编：
 *   - 未定义 RT_USING_DEVICE（Nano 纯内核）：kservice.c 直接调 rt_hw_console_output
 *     和 res_hw_console_getchar
 *   - 定义 RT_USING_DEVICE：kservice.c 只在"没设 console 设备"时回退到
 *     rt_hw_console_output（开机早期正好用得上）；之后走 bsp/drv_usj_dev.c 注册的
 *     字符设备 "usj"。 */

/* 环里还有多少字节没发出去（调试打点用：等它变 0 才算真的送到主机） */
rt_uint32_t s31_usj_tx_pending(void)
{
    return (s_tx_head - s_tx_tail) & USJ_TX_RING_MASK;
}

/* 往发送环形缓冲里塞 len 个字节（'\n' 自动补 '\r'），并立刻尝试灌进 FIFO */
void s31_usj_put_bytes(const char *str, rt_size_t len)
{
    rt_size_t i;

    for (i = 0; i < len; i++) {
        if (str[i] == '\n') {
            usj_ring_put('\r');                      /* 终端要 CRLF */
        }
        usj_ring_put(str[i]);
    }
    s31_usj_tx_pump();
}

void rt_hw_console_output(const char *str)
{
    s31_usj_put_bytes(str, rt_strlen(str));
}

/* 轮询版取字符：只有"没开设备框架"的编译配置下 finsh 才会用到它
 * （开了设备框架走 drv_usj_dev.c 的中断驱动字符设备）。
 * 无条件提供，免得某个 TU 引用它时链接报错。 */
signed char rt_hw_console_getchar(void)
{
    while (!(S31_REG32(S31_USJ_EP1_CONF) & S31_USJ_OUT_EP_DATA_AVAIL)) {
        s31_usj_tx_pump();                           /* 等输入时顺便把发送缓冲排空 */
        rt_thread_mdelay(10);
    }
    return (signed char)(S31_REG32(S31_USJ_EP1) & 0xFFu);
}
