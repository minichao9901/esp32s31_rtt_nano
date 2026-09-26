/*===========================================================================
 * s31_regs.h -- ESP32-S31 裸机用到的寄存器地址（全部来自 ESP-IDF 6.1 头文件）
 *
 * 每个地址后面标了出处，方便回头核对；不引 IDF 头文件，自己写常量。
 *===========================================================================*/
#ifndef S31_REGS_H
#define S31_REGS_H

#include <stdint.h>

#define S31_REG32(addr)      (*(volatile uint32_t *)(uintptr_t)(addr))
#define S31_REG8(addr)       (*(volatile uint8_t  *)(uintptr_t)(addr))
/* 读-改-写的两个小助手（有字段默认值是"1"的寄存器必须用它们，整字写会清掉别的位）*/
#define S31_SETBITS(a, m)    do { S31_REG32(a) |=  (uint32_t)(m); } while (0)
#define S31_CLRBITS(a, m)    do { S31_REG32(a) &= ~(uint32_t)(m); } while (0)

/* ---- 内存窗口（soc/esp32s31/include/soc/soc.h:152 / ld.hp_mem_defs:9）---- */
#define S31_RAM_LOW          0x2F000000u
#define S31_RAM_HIGH         0x2F07AFC0u   /* SRAM_SEG_END：二级 bootloader 保留区起点 */

/* ---- 看门狗 ---- */
#define S31_RTC_WDT_BASE     0x20801000u   /* register/soc/reg_base.h:98 */
#define S31_RTC_WDT_CONFIG0  0x00
#define S31_RTC_WDT_WPROTECT 0x18
#define S31_TIMG0_BASE       0x20580000u   /* register/soc/reg_base.h:71 */
#define S31_TIMG1_BASE       0x20581000u   /* register/soc/reg_base.h:72 */
#define S31_TIMG_WDTCONFIG0  0x48          /* timer_group_reg.h:576 */
#define S31_TIMG_WDTWPROTECT 0x64          /* timer_group_reg.h:582 */
#define S31_WDT_WKEY         0x50D83AA1u

/* ---- USB-Serial/JTAG（msh 控制台）---- */
#define S31_USJ_BASE         0x20391000u   /* register/soc/reg_base.h:38 */
#define S31_USJ_EP1          (S31_USJ_BASE + 0x00)  /* 读=RX 字节，写=TX 字节 */
#define S31_USJ_EP1_CONF     (S31_USJ_BASE + 0x04)
#define S31_USJ_INT_RAW      (S31_USJ_BASE + 0x08)
#define S31_USJ_INT_ST       (S31_USJ_BASE + 0x0c)
#define S31_USJ_INT_ENA      (S31_USJ_BASE + 0x10)
#define S31_USJ_INT_CLR      (S31_USJ_BASE + 0x14)
#define S31_USJ_CONF0        (S31_USJ_BASE + 0x18)
#define S31_USJ_EP1_WR_DONE          (1u << 0)  /* usb_serial_jtag_reg.h:36 */
#define S31_USJ_IN_EP_DATA_FREE      (1u << 1)  /* :45 */
#define S31_USJ_OUT_EP_DATA_AVAIL    (1u << 2)  /* :52 */
#define S31_USJ_INT_RX               (1u << 2)  /* SERIAL_OUT_RECV_PKT_INT（收到主机数据）*/
#define S31_USJ_INT_TX_EMPTY         (1u << 3)  /* SERIAL_IN_EMPTY_INT（TX FIFO 空）*/
#define S31_USJ_CONF0_PAD_ENABLE     (1u << 14) /* :622 */

/* USB 设备块的时钟/复位（CNNT_SYS+0x34，出处 soc/esp32s31/register/soc/cnnt_sys_reg.h:293-315）
 *   bit30 = usb_device_48m_clk_en（**复位默认 1**）    bit31 = usb_device_rst_en（默认 0）
 * 🚨 这个寄存器只能**读-改-写**：整字写 0 会把 bit30 一起清掉 → USB 设备块整块停振 →
 *    CDC 和 JTAG 同时失联、只能物理断电（README §6.17 踩过）。*/
#define S31_CNNT_SYS_BASE            0x20359000u
#define S31_CNNT_USB_DEVICE_CTRL     (S31_CNNT_SYS_BASE + 0x34u)
#define S31_CNNT_USB_48M_CLK_EN      (1u << 30)
#define S31_CNNT_USB_RST_EN          (1u << 31)

/* ---- CLIC（中断控制器）---- */
#define S31_CLIC_BASE        0x10800000u   /* soc/clic_reg.h:17 */
#define S31_CLIC_CTRL_BASE   0x10801000u   /* soc/clic_reg.h:18 */
#define S31_CLIC_THRESH      (S31_CLIC_BASE + 0x8)
/* 每个中断 4 字节：+0 IP、+1 IE、+2 ATTR、+3 CTL（soc/clic_reg.h:114-160） */
#define S31_CLIC_IP(i)       (S31_CLIC_CTRL_BASE + (i) * 4 + 0)
#define S31_CLIC_IE(i)       (S31_CLIC_CTRL_BASE + (i) * 4 + 1)
#define S31_CLIC_ATTR(i)     (S31_CLIC_CTRL_BASE + (i) * 4 + 2)
#define S31_CLIC_CTL(i)      (S31_CLIC_CTRL_BASE + (i) * 4 + 3)
#define S31_CLIC_CTL_PRIO(p) ((uint8_t)((p) << 5))   /* 优先级 3 位，左对齐到 bit7:5 */
#define S31_CLIC_EXT_OFFSET  16             /* soc/clic_reg.h:14：外部中断从 16 号起 */

/* 中断路由矩阵（interrupt_clic_ll.h:41 用的就是这个基址 + 4*src） */
#define S31_INTR0_BASE       0x20585000u    /* register/soc/reg_base.h:77,137 */

/* CLIC 阈值：S31 是标准实现（interrupt_reg.h:28 INTTHRESH_STANDARD=1）
 * → 用 mintthresh CSR(0x347)，值 = NLBITS_TO_BYTE(level) = (level<<5)|0x1F
 *   允许全部优先级 ⇒ level 0 ⇒ 0x1F（riscv/csr_clic.h:59,70） */
#define S31_MINTTHRESH_ALL   0x1Fu

/* ---- SYSTIMER（1ms tick）---- */
#define S31_SYSTIMER_BASE    0x20399000u    /* register/soc/reg_base.h:45 */
#define S31_SYSTIMER_CONF        (S31_SYSTIMER_BASE + 0x00)
#define S31_SYSTIMER_UNIT0_OP    (S31_SYSTIMER_BASE + 0x04)
#define S31_SYSTIMER_UNIT0_LOAD_HI (S31_SYSTIMER_BASE + 0x0c)
#define S31_SYSTIMER_UNIT0_LOAD_LO (S31_SYSTIMER_BASE + 0x10)
#define S31_SYSTIMER_TARGET0_HI  (S31_SYSTIMER_BASE + 0x1c)
#define S31_SYSTIMER_TARGET0_LO  (S31_SYSTIMER_BASE + 0x20)
#define S31_SYSTIMER_TARGET0_CONF (S31_SYSTIMER_BASE + 0x34)
#define S31_SYSTIMER_COMP0_LOAD  (S31_SYSTIMER_BASE + 0x50)
#define S31_SYSTIMER_UNIT0_LOAD  (S31_SYSTIMER_BASE + 0x5c)
#define S31_SYSTIMER_INT_ENA     (S31_SYSTIMER_BASE + 0x64)
#define S31_SYSTIMER_INT_RAW     (S31_SYSTIMER_BASE + 0x68)
#define S31_SYSTIMER_INT_CLR     (S31_SYSTIMER_BASE + 0x6c)
#define S31_SYSTIMER_INT_ST      (S31_SYSTIMER_BASE + 0x70)
#define S31_SYSTIMER_UNIT0_VALUE_HI (S31_SYSTIMER_BASE + 0x40)
#define S31_SYSTIMER_UNIT0_VALUE_LO (S31_SYSTIMER_BASE + 0x44)
/* systimer_reg.h 位定义 */
#define S31_SYSTIMER_CLK_EN           (1u << 31)
#define S31_SYSTIMER_UNIT0_WORK_EN    (1u << 30)
#define S31_SYSTIMER_TARGET0_WORK_EN  (1u << 24)
#define S31_SYSTIMER_UNIT0_OP_UPDATE  (1u << 30)
#define S31_SYSTIMER_UNIT0_VALUE_VALID (1u << 29)
#define S31_SYSTIMER_T0_INT           (1u << 0)
/* TARGET0_CONF：PERIOD[25:0]，PERIOD_MODE=bit30（1=周期），TIMER_UNIT_SEL=bit31（0=UNIT0） */
#define S31_SYSTIMER_TARGET0_PERIOD_MODE  (1u << 30)
#define S31_SYSTIMER_TARGET0_UNIT_SEL     (1u << 31)
/* 时钟源固定 XTAL 40MHz / 2.5 分频 = 16MHz（esp_hw_support/port/esp32s31/systimer.c:10） */
#define S31_SYSTIMER_HZ       16000000u

/* SYSTIMER 时钟门控（hp_sys_clkrst_reg.h:2962，bit0 APB_CLK_EN / bit4 CLK_EN） */
#define S31_HP_SYS_CLKRST_BASE   0x20587000u   /* reg_base.h:79 */
#define S31_SYSTIMER_CTRL0       (S31_HP_SYS_CLKRST_BASE + 0x120)
#define S31_SYSTIMER_APB_CLK_EN  (1u << 0)
#define S31_SYSTIMER_CLK_EN_BIT  (1u << 4)

/* ---- 中断源号（soc/esp32s31/include/soc/interrupts.h 的枚举值）---- */
#define S31_ETS_USB_SERIAL_JTAG     2
#define S31_ETS_SYSTIMER_TARGET0    33

/*===========================================================================
 * PSRAM（2026-09-25 从 boot_msc_s31/bsp/s31_regs.h 合并 —— 移植 bsp/s31_psram.c 需要）
 *
 * 地址出处：IDF components/soc/esp32s31/ld/esp32s31.peripherals.ld
 * （那一串外设实例符号就是 IDF 靠 .ld 提供的，我们不链 IDF，只能自己列）
 *===========================================================================*/
#define S31_MODEM_LPCON_BASE   0x2010f000u  /* +0x18 clk_conf.clk_i2c_mst_en(bit2) = 模拟 I2C 主机时钟 */
#define S31_SPIMEM2_BASE       0x20502000u  /* PSRAM 系统侧控制器（同级还有 SPIMEM3）*/
#define S31_SPIMEM3_BASE       0x20503000u  /* PSRAM 外设侧控制器（读写芯片 mode register 用）*/
#define S31_HP_ALIVE_SYS_BASE  0x20589000u  /* +0x00 hp_clk_ctrl：MPLL 500M 时钟使能 */
#define S31_LP_SYS_BASE        0x20700000u
#define S31_LP_CLKRST_BASE     0x20701000u  /* LP_AONCLKRST：复位原因 / CPLL 分频 */
#define S31_PMU_BASE           0x20704000u  /* PMU：PSRAM 专用 1.8V LDO + MPLL 掉电控制 */

/*===========================================================================
 * USB OTG HS（DWC2 @0x20300000）+ USB-Serial/JTAG 的时钟/复位
 *
 * 2026-09-26 从 boot_msc_s31/bsp/s31_regs.h 合并 —— 那边的 MS​C 盘**真板跑通过**，
 * 这些位号是踩过坑之后核对过的，别凭印象改：
 *   · `CNNT_SYS` 的偏移必须从 IDF 的 struct 头**逐字段累加**（struct 里有
 *     `uint32_t reserved_008[2];` 这种没有 volatile 的字段，漏算就整体偏 8 字节，
 *     把 USB 时钟写到隔壁寄存器上 → 控制器一直没时钟、GHWCFG 全 0）。
 *   · `sys_usb_otg20_ctrl` 的三路复位**默认是"按住"的**，而且按住期间对该寄存器的
 *     写会被吃掉 → 顺序必须是"先放复位（先 PHY 后控制器）→ 再开时钟"。
 *   · `sys_hp_usb_device_ctrl` 的 bit30 复位默认 = 1，**只能读-改-写**：
 *     整字写 0 会把 USB 设备块（CDC + JTAG）一起停振，只能物理断电恢复。
 *===========================================================================*/
#define S31_USB_OTGHS_BASE     0x20300000u  /* DWC2 HS 控制器 */
#define S31_USB_UTMI_BASE      0x20380000u  /* UTMI PHY 控制寄存器 */

/* HP_SYS_CLKRST.usb_otghs_ctrl0 @+0xac：APB/SYS 时钟门控 */
#define S31_CLKRST_USB_OTGHS_CTRL0  (S31_HP_SYS_CLKRST_BASE + 0xacu)
#define S31_USB_CLK_APB_EN          (1u << 0)
#define S31_USB_CLK_SYS_EN          (1u << 1)

/* CNNT_SYS.sys_usb_otg20_ctrl @+0x30 */
#define S31_CNNT_USB_CLK_CTRL       (S31_CNNT_SYS_BASE + 0x2cu)
#define S31_CNNT_USB_OTG20_CTRL     (S31_CNNT_SYS_BASE + 0x30u)
#define S31_USB20_UTMIFS_CLK_EN     (1u << 23)
#define S31_USB20_ULPI_CLK_EN       (1u << 24)
#define S31_USB20_PHYREF_SRC_SEL    (3u << 25)  /* [26:25] 0=12M 1=25M 2=pad */
#define S31_USB20_PHYREF_CLK_EN     (1u << 27)
#define S31_USB20_PHY_RST_EN        (1u << 29)
#define S31_USB20_AHB_RST_EN        (1u << 30)
#define S31_USB20_APB_RST_EN        (1u << 31)

/* HP_ALIVE_SYS.usb_ctrl @+0x24：D+/D- 15k 下拉（设备模式必须断开）*/
#define S31_ALIVE_USB_CTRL          (S31_HP_ALIVE_SYS_BASE + 0x24u)
#define S31_OTGHS_PHY_DMPULLDOWN    (1u << 2)
#define S31_OTGHS_PHY_DPPULLDOWN    (1u << 3)
#define S31_OTGHS_PHY_IDPULLUP      (1u << 4)

/* HP_ALIVE_SYS.usb_otghs_ctrl @+0xb0：PHY PLL / suspendm 手动控制 */
#define S31_ALIVE_USB_OTGHS_CTRL    (S31_HP_ALIVE_SYS_BASE + 0xb0u)
#define S31_OTGHS_PHY_PLL_FORCE_EN  (1u << 0)
#define S31_OTGHS_PHY_PLL_EN        (1u << 1)
#define S31_OTGHS_PHY_SUSPENDM_FORCE_EN (1u << 2)   /* 复位默认=1，必须清 0 交给 DWC2 */
#define S31_OTGHS_PHY_SUSPENDM      (1u << 3)
#define S31_OTGHS_PHY_OTG_SUSPENDM  (1u << 7)

/* LP_SYS.usb_ctrl @+0x100：挂起状态 / 唤醒清除 */
#define S31_LP_SYS_USB_CTRL         (S31_LP_SYS_BASE + 0x100u)
#define S31_LP_USB_WAKEUP_CLR       (1u << 2)
#define S31_LP_USB_IN_SUSPEND       (1u << 3)

/* USB_UTMI.fc_06 @+0x18：LS 模式支持 */
#define S31_UTMI_FC06               (S31_USB_UTMI_BASE + 0x18u)
#define S31_UTMI_FC06_LS_PAR_EN     (1u << 0)
#define S31_UTMI_FC06_LS_KPALV_EN   (1u << 3)

/* DWC2 控制器寄存器偏移（CherryUSB 的 dwc2 驱动自己会用，这里用于自检打印）*/
#define S31_DWC2_GOTGCTL            0x000u
#define S31_DWC2_GSNPSID            0x040u
#define S31_DWC2_GHWCFG2            0x048u
#define S31_DWC2_GUSBCFG            0x00cu
#define S31_DWC2_GINTSTS            0x014u
#define S31_DWC2_GINTMSK            0x018u      /* 全局中断屏蔽（stop 时写 0 掐掉中断源）*/
#define S31_DWC2_DCTL               0x804u
#define S31_DWC2_DSTS               0x808u

/* ---- SYSTIMER TARGET1（音频采样率时钟；TARGET0 被 1ms tick 占了）----
 * 偏移按 IDF `soc/esp32s31/register/soc/systimer_struct.h` 的字段顺序累加得出
 * （每寄存器 4 字节）：conf/unit_op[2]/unit_load_val[2×2]/target_val[3×2]/
 * target_conf[3]/unit_val[2×2]/comp_load[3]/unit_load[2]/int_*……
 * 与已有的 TARGET0 那组偏移（0x1c/0x20/0x34/0x50/0x54…）对得上，可互验。*/
#define S31_SYSTIMER_TARGET1_HI     (S31_SYSTIMER_BASE + 0x24)
#define S31_SYSTIMER_TARGET1_LO     (S31_SYSTIMER_BASE + 0x28)
#define S31_SYSTIMER_TARGET1_CONF   (S31_SYSTIMER_BASE + 0x38)
#define S31_SYSTIMER_COMP1_LOAD     (S31_SYSTIMER_BASE + 0x54)
/* systimer_struct.h: conf.target1_work_en = bit23；int_ena/raw/clr/st 的 target1 = bit1 */
#define S31_SYSTIMER_TARGET1_WORK_EN  (1u << 23)
#define S31_SYSTIMER_T1_INT           (1u << 1)
/* 中断源号（soc/interrupts.h：ETS_SYSTIMER_TARGET0/1/2 = 33/34/35）*/
#define S31_ETS_SYSTIMER_TARGET1      34

/* ---- LEDC（PWM 音频用；LEDC0 @0x20392000，出处 soc/esp32s31/ld/esp32s31.peripherals.ld）
 * 寄存器布局（`soc/ledc_struct.h`，已冻结进 bsp/idf_headers）：
 *   channel_group[0].channel[8]  每个 5 个寄存器：conf0/hpoint/duty_init/conf1/duty_r
 *   timer_group[0].timer[4]      每个 2 个：conf/value
 * 时钟在 HP_SYS_CLKRST.ledc_ctrl0（hp_sys_clkrst_reg.h:3275 → base+0x148）：
 *   bit0 apb_clk_en / bit1 rst_en / bit2 force_norst / bit[4:3] clk_src_sel / bit5 clk_en
 * GPIO 矩阵信号号见 soc/gpio_sig_map.h：LEDC0 通道 0..7 = 126..133 */
#define S31_LEDC0_BASE                 0x20392000u
#define S31_HP_SYS_CLKRST_LEDC_CTRL0   (S31_HP_SYS_CLKRST_BASE + 0x148u)
#define S31_LEDC_CLK_APB_EN            (1u << 0)
#define S31_LEDC_CLK_RST_EN            (1u << 1)
#define S31_LEDC_CLK_FORCE_NORST       (1u << 2)
#define S31_LEDC_CLK_SRC_SEL_S         3
#define S31_LEDC_CLK_SRC_SEL_M         (3u << 3)
#define S31_LEDC_CLK_EN                (1u << 5)
#define S31_LEDC_SIG_OUT_CH0           126u      /* LEDC0_LS_SIG_OUT_PAD_OUT0_IDX */

/* PSRAM 的虚拟地址窗口（soc.h:143 SOC_EXTRAM_LOW/HIGH）*/
#define S31_PSRAM_VADDR        0x50000000u
#define S31_PSRAM_VSIZE        0x04000000u  /* 映射窗口 64MB */

#endif /* S31_REGS_H */
