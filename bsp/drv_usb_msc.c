/*===========================================================================
 * drv_usb_msc.c -- ESP32-S31 的 CherryUSB 对接层 + MSC 盘（板载 flash）
 *
 * 分工（"走官方插件"的关键：官方代码一行没改）
 *   · CherryUSB 本体            rt-thread/components/drivers/usb/cherryusb/ 原样编
 *        core/usbd_core.c       设备协议栈
 *        class/msc/usbd_msc.c   MSC 类（U 盘）
 *        demo/msc_ram_template.c 官方 demo 的 **BLKDEV 版**：盘背后挂一个 RT-Thread
 *                               块设备（`RT_CHERRYUSB_DEVICE_TEMPLATE_MSC_BLKDEV`），
 *                               设备名由 CONFIG_USBDEV_MSC_BLOCK_DEV_NAME 指定 = "onboard0"
 *        osal/usb_osal_rtthread.c  OSAL（线程/信号量）
 *        port/dwc2/usb_dc_dwc2.c   DWC2 控制器驱动（ip 核，与厂商无关）
 *   · 本文件                    只补官方**没有**的那层：S31 的 PHY/时钟/中断 + msh 命令
 *
 * S31 侧硬件要点（寄存器定义全部在 bsp/s31_regs.h 的 "USB OTG HS" 段，出处 IDF LL 头）：
 *   🚨 ① `CNNT_SYS.sys_usb_otg20_ctrl` 的三路复位**默认"按住"**，且按住期间对该寄存器的
 *        写会被吃掉 → 顺序必须"先放复位（先 PHY 后控制器）→ 再开时钟"。反了会让控制器
 *        一直没时钟（GSNPSID/GHWCFG 读回全 0，CherryUSB 报
 *        "device_rx_fifo_size cannot be larger than power_on_value 0" 然后死等）。
 *        该寄存器**只能读-改-写**。
 *   🚨 ② `ALIVE.USB_OTGHS_CTRL` 的 `phy_suspendm_force_en`(bit2)/`phy_pll_force_en`(bit0)
 *        复位默认 = 1（软件强制 suspend）→ 必须清 0，交给 DWC2 自动管。
 *   🚨 ③ 设备模式要**断开 D+/D- 的 15k 下拉**（那是主机模式用的）。
 *   ④ 中断：SoC 源 99（ETS_USB_OTGHS）→ CLIC ID 18（16=systick、17=USJ RX 已占）。
 *      CherryUSB 在 ISR 里只做端点搬运；MSC 的块设备读写在线程里（见 usb_config.h）。
 *
 * 为什么不用 IDF 的 `usb_glue_esp.c`：那份是 IDF 专属（要 esp_intr_alloc /
 * esp_private/usb_phy.h 整套），本工程不链 IDF。
 *
 * msh 用法：
 *   msc start   —— 初始化 USB-HS 并枚举成一个 U 盘（PC 上出现可移动磁盘）
 *   msc stop    —— 关控制器（PHY 进 suspend）
 *   msc status  —— 看寄存器与盘的容量
 *   ⚠️ **盘被 PC 用着的时候别 `mount onboard0 / elm`** —— 两边同时写同一个 FAT 必花。
 *      顺序：PC"安全弹出" → `msc stop` → `mount onboard0 / elm` → 读文件。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include "usbd_core.h"
#include "usb_dwc2_param.h"
#include "s31_regs.h"

#define S31_ETS_USB_OTGHS   99      /* soc/interrupts.h */
#define S31_USB_CLIC_ID     18      /* 16=systick, 17=USJ RX */

/*===========================================================================
 * ① S31 USB-HS 上电（顺序 = boot_msc_s31 真板验证过的那套）
 *===========================================================================*/
static void s31_usb_hw_init(void)
{
    /* 1) 复位脉冲：先 PHY，再控制器 */
    S31_SETBITS(S31_CNNT_USB_OTG20_CTRL,
                S31_USB20_PHY_RST_EN | S31_USB20_AHB_RST_EN | S31_USB20_APB_RST_EN);
    rt_thread_mdelay(2);
    S31_CLRBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_PHY_RST_EN);
    rt_thread_mdelay(2);
    S31_CLRBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_AHB_RST_EN | S31_USB20_APB_RST_EN);
    rt_thread_mdelay(2);

    /* 2) 复位放开之后，才开 UTMI / PHY 参考时钟 */
    S31_SETBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_UTMIFS_CLK_EN | S31_USB20_PHYREF_CLK_EN);

    /* 3) 总线时钟（APB + SYS）*/
    S31_SETBITS(S31_CLKRST_USB_OTGHS_CTRL0, S31_USB_CLK_APB_EN | S31_USB_CLK_SYS_EN);

    /* 4) 把 PHY 的 suspend/PLL 交回 DWC2（清软件强制位，复位默认是 1）*/
    S31_CLRBITS(S31_ALIVE_USB_OTGHS_CTRL,
                S31_OTGHS_PHY_SUSPENDM_FORCE_EN | S31_OTGHS_PHY_PLL_FORCE_EN);

    /* 5) 设备模式：断开 D+/D- 的 15k 下拉 */
    S31_CLRBITS(S31_ALIVE_USB_CTRL, S31_OTGHS_PHY_DPPULLDOWN | S31_OTGHS_PHY_DMPULLDOWN);

    /* 6) UTMI：允许 LS 模式 */
    S31_SETBITS(S31_UTMI_FC06, S31_UTMI_FC06_LS_PAR_EN | S31_UTMI_FC06_LS_KPALV_EN);

    /* 7) 精确的 VBUS/断开检测；8) 清一次挂起/唤醒状态 */
    S31_SETBITS(S31_ALIVE_USB_OTGHS_CTRL, S31_OTGHS_PHY_OTG_SUSPENDM);
    S31_SETBITS(S31_LP_SYS_USB_CTRL, S31_LP_USB_WAKEUP_CLR);
}

/*===========================================================================
 * ② CherryUSB 需要的接口
 *===========================================================================*/
volatile int g_usbd_in_isr;         /* usb_config.h 的日志闸门用 */

/* DWC2 用它算 turnaround：必须是**真实 CPU 频率**（本工程会提到 320MHz，
 * 所以不能在编译期写死）—— msc start 时问 drv_clk 要实测值。*/
uint32_t SystemCoreClock = 40000000u;

rt_uint32_t s31_clk_measured_mhz(void);     /* bsp/drv_clk.c：rdcycle × SYSTIMER 实测 */

void s31_usb_set_system_clock(uint32_t hz)
{
    SystemCoreClock = hz;
}

/* FIFO 划分：IDF 的 S31 param_hs 原值（总 896 words = 3584 字节）。
 * ⚠️ 故意用 **FIFO（从）模式**（device_dma_enable=false）：DWC2 不直接访问内存
 *    → 没有 cache 一致性问题、不用管 PSRAM 对齐。代价是数据靠 CPU/中断搬。*/
static const struct dwc2_user_params s31_param_hs = {
    .phy_type = DWC2_PHY_TYPE_PARAM_UTMI,
    .device_dma_enable = false,
    .device_dma_desc_enable = false,
    .device_rx_fifo_size = (896 - 16 - 128 - 128 - 128 - 128 - 16 - 16),   /* 336 words */
    .device_tx_fifo_size = {
        [0] = 16,  /* 64 byte  */
        [1] = 128, /* 512 byte */
        [2] = 128, /* 512 byte */
        [3] = 128, /* 512 byte */
        [4] = 128, /* 512 byte */
        [5] = 16,  /* 64 byte  */
        [6] = 16,  /* 64 byte  */
        [7] = 0, [8] = 0, [9] = 0, [10] = 0,
        [11] = 0, [12] = 0, [13] = 0, [14] = 0, [15] = 0 },
    .host_dma_desc_enable = false,
    .host_rx_fifo_size = (896 - 128 - 128),
    .host_nperio_tx_fifo_size = 128,
    .host_perio_tx_fifo_size = 128,
};

void dwc2_get_user_params(uint32_t reg_base, struct dwc2_user_params *params)
{
    (void)reg_base;
    rt_memcpy(params, &s31_param_hs, sizeof(struct dwc2_user_params));
}

void usbd_dwc2_delay_ms(uint8_t ms)
{
    rt_thread_mdelay((rt_int32_t)ms);
}

uint32_t usbd_dwc2_get_system_clock(void)
{
    return SystemCoreClock;
}

void usb_dc_low_level_init(uint8_t busid)
{
    (void)busid;
    s31_usb_hw_init();
}

void usb_dc_low_level_deinit(uint8_t busid)
{
    (void)busid;
}

/*===========================================================================
 * ③ 中断：源 99 → CLIC ID 18
 *===========================================================================*/
static void s31_usb_isr(int irq, void *param)
{
    (void)irq;
    (void)param;

    g_usbd_in_isr = 1;
    USBD_IRQHandler(0);
    g_usbd_in_isr = 0;
}

static void s31_usb_irq_install(void)
{
    /* 顺序同 drv_usj_dev.c：先装 handler + 配路由，最后才使能 CLIC
     * （否则"装之前就有挂起中断"会以 NULL handler 进去，没人清标志 → 电平触发死循环）*/
    rt_hw_interrupt_install(S31_USB_CLIC_ID, s31_usb_isr, RT_NULL, "usb-otg");

    S31_REG32(S31_INTR0_BASE + 4u * S31_ETS_USB_OTGHS) = S31_USB_CLIC_ID;
    S31_REG8(S31_CLIC_IP(S31_USB_CLIC_ID))   = 0;
    S31_REG8(S31_CLIC_ATTR(S31_USB_CLIC_ID)) = 0;
    S31_REG8(S31_CLIC_CTL(S31_USB_CLIC_ID))  = S31_CLIC_CTL_PRIO(1);

    __asm__ volatile ("csrw 0x347, %0" : : "r"((rt_uint32_t)S31_MINTTHRESH_ALL));
    __asm__ volatile ("csrs mie, %0" : : "r"(1u << S31_USB_CLIC_ID));
    __asm__ volatile ("csrs mstatus, 8");
    S31_REG8(S31_CLIC_IE(S31_USB_CLIC_ID)) = 1;
}

/*===========================================================================
 * ④ msh：msc start / stop / status
 *    （盘本体是官方 demo 的 msc_ram_init()，我们只负责按需拉起/停下）
 *===========================================================================*/
void msc_ram_init(uint8_t busid, uintptr_t reg_base);       /* demo/msc_ram_template.c */

static int s_msc_started;

static void msc(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "status";

    if (rt_strcmp(sub, "start") == 0) {
        if (s_msc_started) {
            rt_kprintf("[msc] 已经在跑了（先 `msc stop`）\n");
            return;
        }
        rt_kprintf("[msc] 拉起 USB-HS（DWC2 @0x%08x，UTMI PHY，FIFO 模式，CPU %u MHz）...\n",
                   (unsigned)ESP_USB_HS0_BASE, (unsigned)(s31_clk_measured_mhz()));
        /* DWC2 的 turnaround 计算吃这个数 —— 用实测主频，别用 POR 的 40MHz */
        SystemCoreClock = (uint32_t)s31_clk_measured_mhz() * 1000000u;
        s31_usb_irq_install();
        msc_ram_init(0, ESP_USB_HS0_BASE);      /* ← 官方 demo 的入口 */
        s_msc_started = 1;
        rt_kprintf("[msc] 已启动：PC 上应出现可移动磁盘（盘 = onboard0，板载 flash 8MB）\n");
        rt_kprintf("[msc] 用完先让 PC \"安全弹出\"，再 `msc stop`，然后才能 `mount onboard0 / elm`\n");
    } else if (rt_strcmp(sub, "stop") == 0) {
        if (!s_msc_started) {
            rt_kprintf("[msc] 没在跑\n");
            return;
        }
        usbd_deinitialize(0);
        S31_CLRBITS(S31_CNNT_USB_OTG20_CTRL, S31_USB20_UTMIFS_CLK_EN | S31_USB20_PHYREF_CLK_EN);
        S31_SETBITS(S31_ALIVE_USB_OTGHS_CTRL,
                    S31_OTGHS_PHY_SUSPENDM_FORCE_EN | S31_OTGHS_PHY_PLL_FORCE_EN);
        /* 把中断源**也**掐干净（2026-09-26 踩过）：只关 CLIC 的 IE、却让控制器继续
         * assert 中断线的话，CLIC 里会留一个"pending 但被屏蔽"的中断源 ——
         * 之后整个 CLIC 会卡死（音频 + tick 全停在 pending、MIE=1、mintthresh 全开
         * 却再也不进中断）。所以：DWC2 关全局中断 + 清挂起 → 清 CLIC pending → 关 IE。*/
        S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GINTMSK) = 0;
        S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GINTSTS) = 0xFFFFFFFFu;
        S31_REG8(S31_CLIC_IP(S31_USB_CLIC_ID)) = 0;
        S31_REG8(S31_CLIC_IE(S31_USB_CLIC_ID)) = 0;
        __asm__ volatile ("csrc mie, %0" : : "r"(1u << S31_USB_CLIC_ID));
        s_msc_started = 0;
        rt_kprintf("[msc] 已停止（PHY 进 suspend、CLIC 中断已关）\n");
    } else {
        rt_kprintf("msc start|stop|status\n");
        rt_kprintf("  状态      : %s\n", s_msc_started ? "running" : "stopped");
        rt_kprintf("  DWC2      : GSNPSID=0x%08x GHWCFG2=0x%08x DSTS=0x%08x DCTL=0x%08x\n",
                   (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GSNPSID),
                   (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_GHWCFG2),
                   (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DSTS),
                   (unsigned)S31_REG32(S31_USB_OTGHS_BASE + S31_DWC2_DCTL));
        rt_kprintf("  CNNT_CTRL : 0x%08x（bit29 PHY_RST / bit23 UTMI clk / bit27 PHYREF clk）\n",
                   (unsigned)S31_REG32(S31_CNNT_USB_OTG20_CTRL));
        rt_kprintf("  CPU 时钟  : %u Hz（DWC2 turnaround 用）\n", (unsigned)SystemCoreClock);
        rt_kprintf("  盘        : onboard0（16384 x 512B = 8MB，板载 flash 0x400000 起）\n");
    }
}
MSH_CMD_EXPORT(msc, USB MSC disk over onboard flash: msc start|stop|status);
