/*===========================================================================
 * drv_rtt.c -- SEGGER RTT 控制台（走 JTAG 调试口，完全不碰 USB-CDC）
 *
 * 为什么要有这条路（2026-09-26，被 USB-CDC 坑了整整几轮之后）：
 *   USB-Serial/JTAG 那个 CDC 控制台**太容易哑**：主机侧一旦不再收 IN 端点
 *   （或者开端口时的 DTR/RTS 把芯片按进 ROM 下载模式），日志就全丢了 ——
 *   而日志恰恰是查别的问题时唯一的眼睛。RTT 走的是**调试器的内存读写**：
 *   固件只管往 RAM 里的环形缓冲写，主机（OpenOCD `rtt` 命令）在 CPU 跑的
 *   同时把缓冲读走。它不依赖 CDC 的握手、不会被主机"停止消费"卡住，
 *   也不需要独占串口（可以边烧录边看）。
 *
 * 组成（一行源码都没改官方的）：
 *   bsp/segger_rtt/SEGGER_RTT.c / .h / _printf.c   SEGGER 官方实现（见该目录 README）
 *   bsp/segger_rtt/SEGGER_RTT_Conf.h               本工程的配置（缓冲大小、锁）
 *   本文件                                          接进 RT-Thread 的收尾工作
 *
 * 三个挂接点：
 *   ① 输出镜像：`s31_console_mirror()` 由 bsp/drv_usj.c 的 s31_usj_put_bytes()
 *      调用 —— 那是**所有**控制台输出的必经之路（rt_kprintf / rt_hw_console_output
 *      / console 设备 write 三合一），所以一个挂钩点就够。
 *   ② 输入注入：一个低优先级线程轮询 RTT 下行缓冲，把字节塞进 USJ 驱动那
 *      同一个 RX 环（`s31_usj_rx_inject()`）→ finsh 照常被 rx_indicate 唤醒。
 *   ③ msh 命令 `rtt`：看缓冲水位和"主机有没有在取"。
 *
 * 主机侧怎么用（详见 README）：
 *   openocd -f board/esp32s31-builtin.cfg -c "init" \
 *           -c 'rtt setup 0x2F000000 0x80000 "SEGGER RTT"' -c "rtt start" \
 *           -c "rtt server start 9090 0"
 *   然后 telnet/自写脚本连 9090 就是一条双向控制台（tools/rtt.py 封装了这套）。
 *
 * ⚠️ RTT 控制块（_SEGGER_RTT）在 .bss 里，所以 OpenOCD 的搜索范围要覆盖
 *    RAM 窗口 0x2F000000..0x2F07AFC0（linker.ld 的 RAM 区）。
 * ⚠️ 调试器只读 RAM —— 控制块**不需要**任何特殊段摆放，别学 Cortex-M 那套
 *    `SEGGER_RTT_SECTION` 放到 noinit 段的做法。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include "SEGGER_RTT.h"

#define S31_RTT_IN_THREAD_STACK   1024
#define S31_RTT_IN_THREAD_PRIO    25        /* 比 tshell(20) 低：输入注入不抢活 */
#define S31_RTT_IN_POLL_MS        5

/* bsp/drv_usj_dev.c 提供的"往控制台 RX 环里塞字节"入口 */
void s31_usj_rx_inject(const char *buf, rt_size_t len);

static volatile rt_uint32_t s_rtt_in_bytes;
static volatile rt_uint32_t s_rtt_out_bytes;

/* ---- ① 输出镜像（被 bsp/drv_usj.c 的 s31_usj_put_bytes 调用）----------- */
void s31_console_mirror(const char *str, rt_size_t len)
{
    unsigned n;

    if ((str == RT_NULL) || (len == 0u)) {
        return;
    }
    n = SEGGER_RTT_Write(0, str, (unsigned)len);
    s_rtt_out_bytes += n;
}

/* ---- ② 输入注入线程 ---------------------------------------------------- */
static void s31_rtt_in_thread(void *param)
{
    char buf[64];

    (void)param;
    for (;;) {
        unsigned n = SEGGER_RTT_Read(0, buf, sizeof(buf));
        if (n > 0u) {
            rt_size_t i;
            /* 终端发来的换行统一成 '\r'：finsh 只认 '\r' 结束一行 */
            for (i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    buf[i] = '\r';
                }
            }
            s31_usj_rx_inject(buf, n);
            s_rtt_in_bytes += n;
        }
        rt_thread_mdelay(S31_RTT_IN_POLL_MS);
    }
}

/* ---- ③ msh 命令 -------------------------------------------------------- */
static void rtt(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("SEGGER RTT  控制块 @%p（.bss，OpenOCD 在 0x2F000000 起扫 \"SEGGER RTT\"）\n",
               (void *)&_SEGGER_RTT);
    rt_kprintf("  上行 ch0  : %u/%u 字节在用（主机没取走就会显示接近满）\n",
               (unsigned)SEGGER_RTT_GetAvailWriteSpace(0),
               (unsigned)BUFFER_SIZE_UP);
    rt_kprintf("  下行 ch0  : %u 字节收到 / 上行共写出 %u 字节\n",
               (unsigned)s_rtt_in_bytes, (unsigned)s_rtt_out_bytes);
    rt_kprintf("  主机侧    : openocd -c 'rtt setup 0x2F000000 0x80000 \"SEGGER RTT\"' "
               "-c 'rtt start' -c 'rtt server start 9090 0'\n");
}
MSH_CMD_EXPORT(rtt, show segger-rtt console status);

static int s31_rtt_init(void)
{
    rt_thread_t th;

    /* 第一条 RTT 输出：让 OpenOCD 扫到控制块（也证明这条路是通的）*/
    SEGGER_RTT_WriteString(0, "[rtt] SEGGER RTT 已就绪（这条路不经过 USB-CDC）\r\n");

    th = rt_thread_create("rtt-in", s31_rtt_in_thread, RT_NULL,
                          S31_RTT_IN_THREAD_STACK, S31_RTT_IN_THREAD_PRIO, 10);
    if (th != RT_NULL) {
        rt_thread_startup(th);
    } else {
        rt_kprintf("[rtt] 输入线程创建失败（只影响 RTT 输入，输出照常）\n");
    }
    return RT_EOK;
}
INIT_APP_EXPORT(s31_rtt_init);
