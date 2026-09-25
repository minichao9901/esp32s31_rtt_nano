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

/* PSRAM 的虚拟地址窗口（soc.h:143 SOC_EXTRAM_LOW/HIGH）*/
#define S31_PSRAM_VADDR        0x50000000u
#define S31_PSRAM_VSIZE        0x04000000u  /* 映射窗口 64MB */

#endif /* S31_REGS_H */
