/*===========================================================================
 * s31_psram.c -- ESP32-S31 片内 PSRAM（16MB，8 线 DDR，200MHz）的**全部初始化**
 *
 * 这是 PSRAM 这一块的**唯一**文件。对外只暴露下面 5 个函数：
 *
 *     int  s31_psram_init(void);                                    初始化（0 = 成功）
 *     void s31_psram_write_raw(paddr, buf, len);                    写 PSRAM（不过 cache）
 *     void s31_psram_read_raw (paddr, buf, len);                    读 PSRAM（不过 cache）
 *     void s31_psram_invalidate_cache(vaddr, len);                  让 cache 里的旧行失效
 *     uint32_t s31_psram_crc_test(base, bytes);                     自检（0 = 通过）
 *
 *---------------------------------------------------------------------------
 * ★★★ 先读这四条结论，能省掉几天的弯路（完整排查记录见 ../PSRAM.md）
 *
 * 1) 🚨 **写 PSRAM 曾经一律 store access fault，真凶是 PMA —— CPU 的自定义 CSR。**
 *    它不是内存映射寄存器，**任何寄存器 dump 都看不见它**，所以当初把 342 个
 *    寄存器跟 IDF 真值机逐一对完都没找出差异。ROM 把
 *    `[0x40000000, +512MB)`（= cache 的外部存储窗口）标成了 R+X、**没有 W**。
 *    IDF 的二级 bootloader 会调 `esp_cpu_configure_region_protection()` 补上，
 *    我们的裸机 bootloader 没有 → 于是"读得好好的、一写就异常"。
 *    修法见下面的 `pma_grant_write()`。
 *
 * 2) **写进去之后要验证"真的落到 PSRAM 了"，必须用 Cache_WriteBack_Invalidate_Addr**
 *    （先写回再失效）。用 Cache_Invalidate_Addr（直接丢脏行）会得到假阴性；
 *    而且测试数据量要远大于 cache（L1 D-cache = 64KB），否则整块都在 cache 里、
 *    一行都不会被挤出去。
 *
 * 3) **装 App 进 PSRAM 仍然走 s31_psram_write_raw()（原始 MSPI 事务）**，
 *    不走普通 store —— 因为原始路径**绕过 cache**，不会留下陈旧行要清。
 *    它和 IDF 自己往 PSRAM 写参考数据用的是同一个底层接口
 *    （`psram_ctrlr_ll_common_transaction`）。
 *
 * 4) **mode register 必须在"连通性检查"之前写**，而且是**读-改-写**
 *    （IDF `s_init_psram_mode_reg` 就是先读回再改位域）。直接整字写会把
 *    厂商 ID 之类的只读位一起写坏。
 *
 *---------------------------------------------------------------------------
 * 为什么用 IDF 的 LL 函数而不是手写寄存器？
 *
 *   本项目为此付过两次学费：`LP_AONCLKRST_MSPI_DIV.FB_DIV` 实际在 bit[7:3] 而我
 *   写在 bit[4:0]；AP octal PSRAM 的 MR0/MR4/MR8 三个位域我全按印象拍错了
 *   （正确的在 IDF 的 `oct_psram_mode_reg_t` 里）。两次都是"寄存器抄错、不报错、
 *   只是行为不对"，各花掉一整轮。
 *
 *   所以这里**直接用 IDF 的头文件**（hal/psram_ctrlr_ll.h / hal/mspi_ll.h /
 *   hal/mmu_ll.h），并按字段名写，不手抄位号。
 *
 *   ⚠️ 但**不用那些宏壳版本**：`psram_ctrlr_ll_xxx()` 是
 *        `do { (void)__DECLARE_RCC_ATOMIC_ENV; _psram_ctrlr_ll_xxx(...); } while(0)`
 *      套壳宏，逼得调用方到处 `#define __DECLARE_RCC_ATOMIC_ENV 0` 还带一堆
 *      `(void)0;` 警告。**只有 5 个函数有这种壳**（enable_core_clock /
 *      enable_module_clock / reset_module_clock / select_clk_source /
 *      set_core_clock_div），直接调它们的下划线版本即可 ——
 *      IDF 自己的二级 bootloader（bootloader_esp32s31.c）也是这么干的。
 *===========================================================================*/
#include <stdint.h>
#include "s31_regs.h"
#include "s31_psram.h"

#include "hal/psram_ctrlr_ll.h"     /* IDF：PSRAM 控制器 LL（按字段写，不抄位号）*/
#include "hal/mspi_ll.h"            /* IDF：mspi_timing_ll_pin_drv_set / enable_dqs */
#include "hal/mmu_ll.h"             /* IDF：MMU 页表（mmu_hal_map_region 的全部内容）*/
#include "soc/cache_reg.h"          /* 排查用：CACHE_L1_* 寄存器地址 */

#include <rtthread.h>          /* rt_kprintf / rt_uint32_t */

/* us 级忙等：用 SYSTIMER（16MHz）。**故意不用 rt_thread_mdelay** ——
 * PSRAM 初始化在 board init 阶段就会被调用（调度器还没起来），那时 mdelay 不靠谱。*/
static void s31_delay_us(uint32_t us)
{
    extern rt_uint64_t s31_systimer_get_ticks(void);
    rt_uint64_t t0 = s31_systimer_get_ticks();
    while ((s31_systimer_get_ticks() - t0) < (rt_uint64_t)us * (S31_SYSTIMER_HZ / 1000000u)) {
    }
}


/*===========================================================================
 * 1. 常数（每个都标了出处）
 *===========================================================================*/

/* ---- AP octal PSRAM 的命令与时序（IDF esp_psram_impl_ap_oct.c，200MHz 档）----
 * ⚠️ 这些数字必须和 CONFIG_SPIRAM_SPEED_200M 那一档**整套**配套，
 *    不能只改其中一个（dummy 长度是跟着 read/write latency 走的）。 */
#define AP_RD_CMD_BITLEN        16
#define AP_WR_CMD_BITLEN        16
#define AP_ADDR_BITLEN          32
#define AP_RD_DUMMY_BITLEN      (2 * (14 - 1))   /* 26：读 latency 4 -> 14 cycles @DDR */
#define AP_WR_DUMMY_BITLEN      (2 * (7 - 1))    /* 12：写 latency 1 ->  7 cycles @DDR */
#define AP_RD_REG_DUMMY_BITLEN  (2 * (7 - 1))    /* 12：读 mode register 固定 7 cycles */
#define AP_RD_LATENCY           4
#define AP_WR_LATENCY           1

#define AP_SYNC_READ            0x0000           /* 阵列读 */
#define AP_SYNC_WRITE           0x8080           /* 阵列写 */
#define AP_REG_READ             0x4040           /* mode register 读 */
#define AP_REG_WRITE            0xC0C0           /* mode register 写 */
#define AP_VENDOR_ID_AP         0x0D             /* APS6408 系列 */
#define AP_REF_DATA             0x5a6b7c8d       /* IDF 用的连接性参考字 */

/* CS 时序（寄存器里存的是 n-1，LL 函数会自己减）*/
#define AP_CS_SETUP_TIME        4
#define AP_CS_HOLD_TIME         4
#define AP_CS_HOLD_DELAY        3

/* ---- 时钟 ----
 * MPLL 必须给 400MHz：PSRAM 跑 200MHz 是靠 "MPLL / 2" 分频来的
 * （psram_ctrlr_ll_set_bus_clock(id, MPLL_FREQ / SPEED)）。500/200 除不尽。
 * 实测 IDF 正常态 LP_AONCLKRST_MSPI_DIV 的 FB_DIV = 19 -> (19+1)*40/2 = 400MHz。 */
#define PSRAM_MPLL_FREQ_MHZ     400
#define PSRAM_SPEED_MHZ         200

/* ---- 模拟 I2C 主机时钟（regi2c）----
 * MPLL 自校准靠它比较频率，不开就永远等不到 CAL_END。
 * S31 的 soc_caps 里 ANA_I2C_MST_CLK_HAS_ROOT_GATING=1 -> 默认关着，
 * 必须显式打开（IDF 里叫 ANALOG_CLOCK_ENABLE()）。IDF 在 bootloader 阶段
 * 是"一直开着不关"，这里也只开不关。 */
#define R_MODEM_LPCON_CLK_CONF      (S31_MODEM_LPCON_BASE + 0x18u)
#define B_MODEM_LPCON_CLK_I2C_MST   (1u << 2)
#define R_MODEM_LPCON_CLK_CONF2     (S31_MODEM_LPCON_BASE + 0x1cu)
#define B_MODEM_LPCON_CLK_I2C_MST_FO (1u << 2)
#define R_HP_SYS_CLKRST_MODEM       (S31_HP_SYS_CLKRST_BASE + 0x40u)
#define B_MODEM_BUS_CLK             (1u << 0)

/* ---- PSRAM/MPLL 专用 1.8V LDO（S31 的 SOC_PSRAM_HAS_DEDICATED_LDO=1）----
 * ★ 这是早期"MPLL 永远不收敛 + PSRAM 事务读到垃圾"的真凶：PSRAM 与 MPLL PHY
 *   挂在一个**专用 1.8V LDO** 上，上电默认是关的（PMU.psram_cfg.psram_xpd=0、
 *   整个 ext_ldo_ctrl 是 0）。不开它 MPLL 没供电、校准比较器不工作、
 *   PSRAM 芯片也没有 I/O 电平。
 * 顺序照 IDF 的 esp_ldo_channel_acquire()（chan 1 -> unit 0，1800mV）：
 *   限流开 -> 设电压 -> 开纹波抑制 -> 使能 psram_xpd -> 限流关 -> 等 1ms
 * dref/mul 用 IDF 的 ldo_ll_voltage_to_dref_mul()：1800mV = dref 6 / mul 5。
 * 和 IDF 一样只开不关（PSRAM 一关就没了）。 */
#define R_PMU_EXT_LDO_CTRL          (S31_PMU_BASE + 0x218u)
#define B_EXT_LDO_TIE_HIGH          (1u << 0)
#define B_EXT_LDO_EN_VDET           (1u << 1)
#define B_EXT_CUR_LIM               (1u << 2)
#define S_EXT_LDO_MUL               3u
#define M_EXT_LDO_MUL               0x07u
#define S_EXT_LDO_DREF              6u
#define M_EXT_LDO_DREF              0x0Fu
#define R_PMU_PSRAM_CFG             (S31_PMU_BASE + 0x1e8u)
#define B_PSRAM_XPD                 (1u << 31)

/* ---- MPLL（clk_ll_mpll_enable / set_config / calibration）---- */
#define R_PMU_IMM_HP_CK_POWER_1     (S31_PMU_BASE + 0xf4u)
#define B_TIE_HIGH_XPD_MPLL         (1u << 30)
#define B_TIE_HIGH_XPD_MPLL_I2C     (1u << 26)
#define B_GLOBAL_MPLL_ICG           (1u << 22)
#define R_HP_ALIVE_SYS_HP_CLK_CTRL  (S31_HP_ALIVE_SYS_BASE + 0x00u)
#define B_HP_MPLL_500M_CLK_EN       (1u << 31)
#define R_PMU_HP_ACTIVE_HP_CK_POWER (S31_PMU_BASE + 0x1cu)
#define B_HP_ACTIVE_XPD_MPLL        (1u << 30)
#define B_HP_ACTIVE_XPD_MPLL_I2C    (1u << 26)
#define R_LP_AON_MSPI_DIV           (S31_LP_CLKRST_BASE + 0x54u)
/* 🚨 字段布局（LP_AONCLKRST_MSPI_DIV_REG）：
 *      REF_DIV bits[2:0]（默认 1）
 *      FB_DIV  bits[7:3]（默认 24）  ← **不是** bits[4:0]！
 *      CHGP_DCUR bit[8]
 * 早先按 bits[4:0] 写，等于把 REF_DIV 打成 0 且只改到 FB_DIV 的低 2 位
 * （24 -> 27）→ MPLL 目标变成 (27+1)*40/1 = 1120MHz，自校准永远不收敛。 */
#define S_MSPI_FB_DIV               3u
#define M_MSPI_FB_DIV               0x1Fu
#define R_HP_CLKRST_ANA_PLL_CTRL0   (S31_HP_SYS_CLKRST_BASE + 0x174u)
#define B_MSPI_CAL_END              (1u << 8)
#define B_MSPI_CAL_STOP             (1u << 9)

/* ---- cache ----
 * HP_SYS_CLKRST.cache_ctrl0 @+0x38：四位"时钟强制常开"。
 * IDF 的 cache_hal_init() 第一步 cache_ll_clk_init() 就是设这四位；
 * 没有它们硬件会认为 cache 空闲就把时钟门掉。 */
#define R_HP_SYS_CLKRST_CACHE_CTRL0 (S31_HP_SYS_CLKRST_BASE + 0x38u)

/* cache 的"失效"用 ROM 函数；map 位出自 rom/cache.h：
 *   CACHE_MAP_L1_ICACHE_0 = BIT(0) / L1_ICACHE_1 = BIT(1) / L1_DCACHE = BIT(4)
 * ★ 必须 I/D 一起失效：App 的 .text 也在 PSRAM，只失效 D-cache 可能执行到陈旧指令。 */
#define CACHE_MAP_ALL               ((1u << 0) | (1u << 1) | (1u << 4))
extern int Cache_Invalidate_Addr(uint32_t map, uint32_t addr, uint32_t size);
extern void Cache_Enable_L1_CORE0_ICache(uint32_t autoload);
extern void Cache_Enable_L1_DCache(uint32_t autoload);

/* MMU：64KB 页（SOC_MMU_PAGE_SIZE），PSRAM 虚拟窗口从 0x50000000 起 */
#define PSRAM_MMU_PAGE              0x10000u

/*===========================================================================
 * PMA（Physical Memory Attribute）—— **"cache 写 PSRAM 一律 store access fault"的真凶**
 *
 * 这颗 CPU 有 PMA，IDF 把它实现成**自定义 CSR**（不是内存映射寄存器！）：
 *     CSR_PMACFG(i)  = 0xBC0 + i      （i = 0..15）
 *     CSR_PMAADDR(i) = 0xBD0 + i
 * 位定义出自 components/riscv/include/riscv/csr.h：
 *     PMA_EN      = BIT(0)         使能
 *     PMA_W       = BIT(3)         **写**     ← 就是它没被置上
 *     PMA_X       = BIT(2)         执行
 *     PMA_R       = BIT(4)         读
 *     PMA_L       = BIT(29)        锁定
 *     PMA_NAPOT   = 0xC0000000     地址编码方式（TOR = 0x40000000 / NA4 = 0x80000000）
 *     PMA_SHIFT   = 2
 *     PMAADDR 里存的是 NAPOT 掩码形式：((ADDR | ((SIZE>>1)-1)) >> 2)
 *
 * 本板 ROM 留下的现场（实测 dump 出来的）：
 *     PMA12: cfg=C0000019 addr=0x0B800FFF -> [0x2E000000, +32KB)   LP/RTC RAM   R+W
 *     PMA13: cfg=C0000015 addr=0x0BE0FFFF -> [0x2F800000, +512KB)  ROM          R+X
 *     PMA14: cfg=C000001D addr=0x0BC0FFFF -> [0x2F000000, +512KB)  IRAM         R+W+X
 *     PMA15: cfg=C0000015 addr=0x13FFFFFF -> [0x40000000, +512MB)  **外部存储** R+X  ← ★没有 W
 *
 *     [0x40000000, +512MB) 就是 cache 的外部存储虚拟窗口 —— **flash 映射窗口和
 *     PSRAM 窗口都在这一段里**。ROM 把整段标成了只读，于是：
 *         读全对、能执行（App 的 .text 就是从 PSRAM 取指跑的）、
 *         **而任何 store 一律 store access fault**，连 flash 窗口和 PSRAM 窗口
 *         的表现都一模一样，因为对 PMA 来说它们是同一段。
 *
 * ★ 为什么查了那么久：**PMA 是 CSR，不在内存映射空间里**，所以任何"寄存器对差"
 *   都看不到它。本项目拿 IDF 当真值机、逐块比过 342 个寄存器
 *   （CACHE / HP_SYS_CLKRST / 三个 MSPI / PMP / MMU / CPU_APM / HP_APM / TEE / PMS）
 *   全都对得上，才逼出"一定是非内存映射状态"这个结论，最后在
 *   IDF 的 riscv/csr.h 里找到 PMACFG/PMAADDR。
 *
 * IDF 侧的权威写法在 components/esp_hw_support/port/esp32s31/cpu_region_protect.c，
 * 它自己的注释就写着 "without setting this, psram cannot be reached"：
 *     PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW, SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW,
 *                                  PMA_NAPOT | PMA_RWX);
 * 这个函数由**二级 bootloader** 调用（bootloader_mem.c:63）—— 本工程的裸机
 * bootloader 顶掉了二级 bootloader，所以这一步必须自己补。
 *
 * 这里用**更保守**的做法（不重置整张表 —— 我们正从 IRAM 里跑，重置 PMA 有风险）：
 *   扫一遍 16 条，凡是"使能的 NAPOT 条目且区间覆盖 PSRAM 窗口"的，补上 PMA_W。
 *===========================================================================*/
#define PMA_EN      0x00000001u
#define PMA_X       0x00000004u
#define PMA_W       0x00000008u
#define PMA_R       0x00000010u
#define PMA_NAPOT   0xC0000000u

/* CSR 号必须是汇编里的立即数，所以只能这样展开（16 条，写全了最直观）*/
static uint32_t pma_cfg_read(unsigned i)
{
    uint32_t v = 0;
    switch (i) {
    case  0: __asm__ volatile ("csrr %0, 0xbc0" : "=r"(v)); break;
    case  1: __asm__ volatile ("csrr %0, 0xbc1" : "=r"(v)); break;
    case  2: __asm__ volatile ("csrr %0, 0xbc2" : "=r"(v)); break;
    case  3: __asm__ volatile ("csrr %0, 0xbc3" : "=r"(v)); break;
    case  4: __asm__ volatile ("csrr %0, 0xbc4" : "=r"(v)); break;
    case  5: __asm__ volatile ("csrr %0, 0xbc5" : "=r"(v)); break;
    case  6: __asm__ volatile ("csrr %0, 0xbc6" : "=r"(v)); break;
    case  7: __asm__ volatile ("csrr %0, 0xbc7" : "=r"(v)); break;
    case  8: __asm__ volatile ("csrr %0, 0xbc8" : "=r"(v)); break;
    case  9: __asm__ volatile ("csrr %0, 0xbc9" : "=r"(v)); break;
    case 10: __asm__ volatile ("csrr %0, 0xbca" : "=r"(v)); break;
    case 11: __asm__ volatile ("csrr %0, 0xbcb" : "=r"(v)); break;
    case 12: __asm__ volatile ("csrr %0, 0xbcc" : "=r"(v)); break;
    case 13: __asm__ volatile ("csrr %0, 0xbcd" : "=r"(v)); break;
    case 14: __asm__ volatile ("csrr %0, 0xbce" : "=r"(v)); break;
    case 15: __asm__ volatile ("csrr %0, 0xbcf" : "=r"(v)); break;
    default: break;
    }
    return v;
}

static void pma_cfg_write(unsigned i, uint32_t v)
{
    switch (i) {
    case  0: __asm__ volatile ("csrw 0xbc0, %0" : : "r"(v)); break;
    case  1: __asm__ volatile ("csrw 0xbc1, %0" : : "r"(v)); break;
    case  2: __asm__ volatile ("csrw 0xbc2, %0" : : "r"(v)); break;
    case  3: __asm__ volatile ("csrw 0xbc3, %0" : : "r"(v)); break;
    case  4: __asm__ volatile ("csrw 0xbc4, %0" : : "r"(v)); break;
    case  5: __asm__ volatile ("csrw 0xbc5, %0" : : "r"(v)); break;
    case  6: __asm__ volatile ("csrw 0xbc6, %0" : : "r"(v)); break;
    case  7: __asm__ volatile ("csrw 0xbc7, %0" : : "r"(v)); break;
    case  8: __asm__ volatile ("csrw 0xbc8, %0" : : "r"(v)); break;
    case  9: __asm__ volatile ("csrw 0xbc9, %0" : : "r"(v)); break;
    case 10: __asm__ volatile ("csrw 0xbca, %0" : : "r"(v)); break;
    case 11: __asm__ volatile ("csrw 0xbcb, %0" : : "r"(v)); break;
    case 12: __asm__ volatile ("csrw 0xbcc, %0" : : "r"(v)); break;
    case 13: __asm__ volatile ("csrw 0xbcd, %0" : : "r"(v)); break;
    case 14: __asm__ volatile ("csrw 0xbce, %0" : : "r"(v)); break;
    case 15: __asm__ volatile ("csrw 0xbcf, %0" : : "r"(v)); break;
    default: break;
    }
}

static uint32_t pma_addr_read(unsigned i)
{
    uint32_t v = 0;
    switch (i) {
    case  0: __asm__ volatile ("csrr %0, 0xbd0" : "=r"(v)); break;
    case  1: __asm__ volatile ("csrr %0, 0xbd1" : "=r"(v)); break;
    case  2: __asm__ volatile ("csrr %0, 0xbd2" : "=r"(v)); break;
    case  3: __asm__ volatile ("csrr %0, 0xbd3" : "=r"(v)); break;
    case  4: __asm__ volatile ("csrr %0, 0xbd4" : "=r"(v)); break;
    case  5: __asm__ volatile ("csrr %0, 0xbd5" : "=r"(v)); break;
    case  6: __asm__ volatile ("csrr %0, 0xbd6" : "=r"(v)); break;
    case  7: __asm__ volatile ("csrr %0, 0xbd7" : "=r"(v)); break;
    case  8: __asm__ volatile ("csrr %0, 0xbd8" : "=r"(v)); break;
    case  9: __asm__ volatile ("csrr %0, 0xbd9" : "=r"(v)); break;
    case 10: __asm__ volatile ("csrr %0, 0xbda" : "=r"(v)); break;
    case 11: __asm__ volatile ("csrr %0, 0xbdb" : "=r"(v)); break;
    case 12: __asm__ volatile ("csrr %0, 0xbdc" : "=r"(v)); break;
    case 13: __asm__ volatile ("csrr %0, 0xbdd" : "=r"(v)); break;
    case 14: __asm__ volatile ("csrr %0, 0xbde" : "=r"(v)); break;
    case 15: __asm__ volatile ("csrr %0, 0xbdf" : "=r"(v)); break;
    default: break;
    }
    return v;
}

/* 从 PMAADDR 反推 NAPOT 区间。
 * PMAADDR = ((ADDR | ((SIZE>>1)-1)) >> 2)，所以尾部连续 1 的个数 k 满足
 *     (SIZE>>1)-1 = (1<<(k+2))-1   ->   SIZE = 1 << (k+3)
 *     base = (PMAADDR & ~((1<<k)-1)) << 2                                   */
static int pma_napot_range(uint32_t adr, uint32_t *base, uint32_t *size)
{
    uint32_t k = 0;

    while (k < 31u && (adr & (1u << k))) {
        k++;
    }
    if (k == 0u || k >= 31u) {
        return -1;                      /* 不是合法的 NAPOT 编码 */
    }
    *size = 1u << (k + 3u);
    *base = (adr & ~((1u << k) - 1u)) << 2;
    return 0;
}

/* 给"覆盖 PSRAM 窗口"的 PMA 条目补上写权限。返回改了几条。 */
static int pma_grant_write(uint32_t addr)
{
    unsigned i;
    int changed = 0;

    for (i = 0; i < 16u; i++) {
        uint32_t cfg = pma_cfg_read(i);
        uint32_t base, size;

        if (!(cfg & PMA_EN) || (cfg & 0xC0000000u) != PMA_NAPOT) {
            continue;                   /* 没使能 / 不是 NAPOT -> 不管 */
        }
        if (cfg & PMA_W) {
            continue;                   /* 本来就可写 */
        }
        if (pma_napot_range(pma_addr_read(i), &base, &size) != 0) {
            continue;
        }
        if (addr < base || addr >= base + size) {
            continue;                   /* 不覆盖我们关心的窗口 */
        }
        pma_cfg_write(i, cfg | PMA_W);
        rt_kprintf("[psram] PMA%u [%08x,+%uKB) 补上写权限: %08x -> %08x\r\n",
                i, (unsigned)base, (unsigned)(size >> 10),
                (unsigned)cfg, (unsigned)(cfg | PMA_W));
        changed++;
    }
    return changed;
}


/* 初始化结果（成功 = mode register 写通 + 参考字对 + MMU 有效 + 交叉验证通过）*/
static int s_psram_ok;

/*===========================================================================
 * 2. 供电与模拟子系统前置
 *===========================================================================*/

/* PSRAM/MPLL 专用 1.8V LDO + MPLL PHY 上电 */
static void psram_ldo_enable(void)
{
    uint32_t v;

    S31_REG32(R_PMU_EXT_LDO_CTRL) |= B_EXT_CUR_LIM;      /* 先限流，防 inrush */
    v = S31_REG32(R_PMU_EXT_LDO_CTRL);
    v &= ~(M_EXT_LDO_MUL << S_EXT_LDO_MUL);
    v &= ~(M_EXT_LDO_DREF << S_EXT_LDO_DREF);
    v &= ~B_EXT_LDO_TIE_HIGH;                            /* 不用 bypass(3.3V) */
    v |= (5u << S_EXT_LDO_MUL);                          /* 1800mV: mul = 5 */
    v |= (6u << S_EXT_LDO_DREF);                         /* 1800mV: dref = 6 */
    v |= B_EXT_LDO_EN_VDET;                              /* 纹波抑制 */
    S31_REG32(R_PMU_EXT_LDO_CTRL) = v;

    S31_REG32(R_PMU_PSRAM_CFG) |= B_PSRAM_XPD;           /* MPLL PHY 上电 */
    S31_REG32(R_PMU_EXT_LDO_CTRL) &= ~B_EXT_CUR_LIM;
    s31_delay_us(1000);
}

/* 模拟/regi2c 子系统时钟。
 * 出处：bootloader_support/src/esp32s31/bootloader_esp32s31.c 的
 *       bootloader_hardware_init()。本工程用自己的 startup 顶掉了二级 bootloader，
 *       所以这几步必须自己补 —— 缺了它们 regi2c 访问和 PLL 自校准都拿不到时钟。 */
static void psram_ana_prereq(void)
{
    S31_REG32(R_HP_SYS_CLKRST_MODEM) |= B_MODEM_BUS_CLK;          /* modem 寄存器总线时钟 */
    S31_REG32(R_MODEM_LPCON_CLK_CONF) |= B_MODEM_LPCON_CLK_I2C_MST;   /* clk_i2c_mst_en */
    S31_REG32(R_MODEM_LPCON_CLK_CONF2) |= B_MODEM_LPCON_CLK_I2C_MST_FO; /* 强制使能 */
    S31_REG32(0x20109c04u) |= (1u << 12);    /* MODEM_SYSCON：i2c mst 时钟选 160M */
}

/*===========================================================================
 * 3. MPLL -> 400MHz（PSRAM 200MHz = MPLL/2）
 *===========================================================================*/
static int s_mpll_ok;

static void psram_mpll_init(void)
{
    uint32_t fb_div = PSRAM_MPLL_FREQ_MHZ * 2u / 40u - 1u;   /* ref_div = 1 -> 19 */
    uint32_t cal = S31_REG32(R_HP_CLKRST_ANA_PLL_CTRL0);
    uint32_t guard;

    /* clk_ll_mpll_enable()：只做 enable 那一遍（IDF 的正常上电路径也只有它）*/
    S31_REG32(R_PMU_IMM_HP_CK_POWER_1) |=
        (B_GLOBAL_MPLL_ICG | B_TIE_HIGH_XPD_MPLL | B_TIE_HIGH_XPD_MPLL_I2C);
    S31_REG32(R_HP_ALIVE_SYS_HP_CLK_CTRL) |= B_HP_MPLL_500M_CLK_EN;
    S31_REG32(R_PMU_HP_ACTIVE_HP_CK_POWER) |=
        (B_HP_ACTIVE_XPD_MPLL | B_HP_ACTIVE_XPD_MPLL_I2C);
    s31_delay_us(100);

    S31_REG32(R_MODEM_LPCON_CLK_CONF) |= B_MODEM_LPCON_CLK_I2C_MST;

    /* clk_ll_mpll_set_config()：分频比（不管走不走校准，都必须先写对）*/
    {
        uint32_t v = S31_REG32(R_LP_AON_MSPI_DIV);
        v = (v & ~(M_MSPI_FB_DIV << S_MSPI_FB_DIV)) | (fb_div << S_MSPI_FB_DIV);
        S31_REG32(R_LP_AON_MSPI_DIV) = v;
    }

    /* 上一份固件/ROM 已经把 MPLL 校准好（CAL_END=1）就不用再跑一遍。
     * ⚠️ CAL_END 是**跨复位保留**的（LP/AON 域），所以"跑过 IDF 固件之后本工程
     *    的 MPLL 初始化就不灵了"曾经是个坑；现在有 CAL_END 判断 + 超时兜底。 */
    if (cal & B_MSPI_CAL_END) {
        S31_REG32(R_HP_CLKRST_ANA_PLL_CTRL0) |= B_MSPI_CAL_STOP;
        s_mpll_ok = 1;
        return;
    }

    S31_REG32(R_HP_CLKRST_ANA_PLL_CTRL0) &= ~B_MSPI_CAL_STOP;   /* calibration_start */
    guard = 1000000u;
    while (!(S31_REG32(R_HP_CLKRST_ANA_PLL_CTRL0) & B_MSPI_CAL_END) && --guard) {
        /* 空转等自校准。⚠️ 这里**不能**放日志：320MHz 下最坏 ~1e6 次，
         * 日志会把这段时间从"看着卡住"变成"真的卡住"。 */
    }
    S31_REG32(R_HP_CLKRST_ANA_PLL_CTRL0) |= B_MSPI_CAL_STOP;    /* calibration_stop */
    s_mpll_ok = (guard != 0) ? 1 : 0;      /* 超时也不致命：CAL_END 可能本来就保留着 */
}

/*===========================================================================
 * 4. 控制器配置（逐条照 IDF 的 esp_psram_impl_enable()）
 *===========================================================================*/

/* PSRAM 控制器的两个"实例"：
 *   ID_2 = 走 cache/AXI 的那一侧（SPI_MEM_S_* 寄存器）
 *   ID_3 = 手工事务那一侧（SPI1_MEM_S_* 寄存器），配 mode register / 搬数据用它 */
#define PSRAM_ID_CACHE   PSRAM_CTRLR_LL_MSPI_ID_2
#define PSRAM_ID_XFER    PSRAM_CTRLR_LL_MSPI_ID_3

/* ---- 原始 MSPI 事务：**唯一能在本芯片上写 PSRAM 的路径** ----
 * 底层是 ROM 的 esp_rom_spi_cmd_config/esp_rom_spi_cmd_start（符号在 linker.ld 里
 * PROVIDE 成 ROM 地址）。数据一次最多 64 字节 = MSPI FIFO 深度。
 * ⚠️ 必须在 config_mspi_for_psram() **之后**调用：那一步才把控制器切成 DDR+8 线，
 *    dummy 长度也是它设的。 */
#define PSRAM_RAW_CHUNK  64u

static void psram_xfer(uint32_t cmd, uint32_t cmd_bits, uint32_t addr,
                       uint32_t dummy, const void *mosi, uint32_t mosi_bits,
                       void *miso, uint32_t miso_bits)
{
    psram_ctrlr_ll_common_transaction(PSRAM_ID_XFER, cmd, cmd_bits, addr, AP_ADDR_BITLEN,
                                      dummy, (uint8_t *)mosi, mosi_bits,
                                      (uint8_t *)miso, miso_bits, false);
}

void s31_psram_write_raw(uint32_t paddr, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len) {
        uint32_t n = (len > PSRAM_RAW_CHUNK) ? PSRAM_RAW_CHUNK : len;
        psram_xfer(AP_SYNC_WRITE, AP_WR_CMD_BITLEN, paddr, AP_WR_DUMMY_BITLEN,
                   p, n * 8u, 0, 0);
        p += n;
        paddr += n;
        len -= n;
    }
}

void s31_psram_read_raw(uint32_t paddr, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;

    while (len) {
        uint32_t n = (len > PSRAM_RAW_CHUNK) ? PSRAM_RAW_CHUNK : len;
        psram_xfer(AP_SYNC_READ, AP_RD_CMD_BITLEN, paddr, AP_RD_DUMMY_BITLEN,
                   0, 0, p, n * 8u);
        p += n;
        paddr += n;
        len -= n;
    }
}

/* ---- 写 mode register ----
 * ★★★ 这里是整场 PSRAM 攻坚的转折点：**位域必须照 IDF 的 oct_psram_mode_reg_t**，
 *     不能按印象拍。我原来三个全写错了，症状极具欺骗性 ——
 *     寄存器读写全对、回读逐位一致，**只有阵列读写是垃圾**。
 *
 *     IDF 的位域（C 位域自低位排起）：
 *       mr0: drive_str[1:0]  read_latency[4:2]  lt[5]  rsvd6[6]  tso[7]
 *       mr4: pasr[2:0]       rf[4:3]            wr_latency[7:5]
 *       mr8: bl[1:0]  bt[2]  rbx[3]  rsvd5[5:4] x16[6]  rsvd7[7]
 *     200MHz 档要的值 = MR0 (4<<2)|(1<<5)=0x30、MR4 1<<5=0x20、MR8 3|(1<<3)=0x0B
 *
 *     做法也照 IDF 的 s_init_psram_mode_reg()：**读-改-写**（只改指定字段，其余原样写回）。
 *     （以前因为"读 MR0 得到 0x8d 和 IDF 的 0x0d 对不上"而误判成"读相位偏了一位"，
 *       改成"常数构造 + 读到 0x8d 就拒绝写"，反而造出一个死循环。
 *       真相：16 位读一次填两个字节，mr0=低字节、mr1=高字节，
 *       而 vendor_id 只是 mr1 的**低 5 位** —— 0x8D & 0x1F = 0x0D = AP，读本来就是准的。） */
static void psram_write_mode_reg(void)
{
    uint8_t mr01[2] = { 0, 0 };     /* [0]=MR0 [1]=MR1(vendor id) */
    uint8_t mr45[2] = { 0, 0 };     /* [0]=MR4 [1]=MR5 */
    uint8_t mr8 = 0;

    psram_xfer(AP_REG_READ, 16, 0x0, AP_RD_REG_DUMMY_BITLEN, 0, 0, mr01, 16);
    psram_xfer(AP_REG_READ, 16, 0x4, AP_RD_REG_DUMMY_BITLEN, 0, 0, mr45, 16);
    psram_xfer(AP_REG_READ, 16, 0x8, AP_RD_REG_DUMMY_BITLEN, 0, 0, &mr8, 8);

    if ((mr01[1] & 0x1Fu) == AP_VENDOR_ID_AP) {
        rt_kprintf("[psram] 芯片 = AP 系列 (vendor 0x0d)\r\n");
    }

    /* MR0：只改 read_latency[4:2] 与 lt[5]，drive_str 保持读到的值 */
    mr01[0] = (uint8_t)((mr01[0] & ~0x3Cu) | ((AP_RD_LATENCY & 0x7u) << 2) | (1u << 5));
    /* MR4：只改 wr_latency[7:5]，低 5 位（pasr/rf）保留 */
    mr45[0] = (uint8_t)((mr45[0] & 0x1Fu) | ((AP_WR_LATENCY & 0x7u) << 5));
    /* MR8：bl=3、bt=0、rbx=1，高 4 位保留 */
    mr8 = (uint8_t)((mr8 & 0xF0u) | 0x03u | (1u << 3));

    psram_xfer(AP_REG_WRITE, 16, 0x0, 0, mr01, 16, 0, 0);   /* MR0+MR1 一起写 */
    psram_xfer(AP_REG_WRITE, 16, 0x4, 0, mr45, 16, 0, 0);
    psram_xfer(AP_REG_WRITE, 16, 0x8, 0, &mr8, 8, 0, 0);

    rt_kprintf("[psram] mode reg: MR0=%02x MR4=%02x MR8=%02x（期望 %02x/%02x/%02x）\r\n",
            (unsigned)mr01[0], (unsigned)mr45[0], (unsigned)mr8,
            0x30u, 0x20u, 0x0Bu);
}

/* 连接性检查：写一个参考字再读回来。IDF 用它判断"PSRAM 芯片在不在线上"。
 * ★ 必须把读到的值打出来：MISMATCH 有四种完全不同的读法
 *     got == 0x00000000 -> 写没落地 / 没有器件响应
 *     got == 0xAAAAAAAA -> 每个 bit 被采两遍（采样点错位整一轮）
 *     got == ref 平移 N 位 -> 延迟差 N 个 bit
 *     got == ref        -> 通 */
static int psram_check_connected(void)
{
    uint32_t ref = AP_REF_DATA, got = 0;

    psram_xfer(AP_SYNC_WRITE, AP_WR_CMD_BITLEN, 0, AP_WR_DUMMY_BITLEN, &ref, 32, 0, 0);
    psram_xfer(AP_SYNC_READ,  AP_RD_CMD_BITLEN, 0, AP_RD_DUMMY_BITLEN, 0, 0, &got, 32);

    rt_kprintf("[psram] 参考字: 写 %08x 读 %08x -> %s\r\n",
            (unsigned)ref, (unsigned)got, (got == ref) ? "OK" : "MISMATCH");
    return (got == ref) ? 0 : -1;
}

/* 芯片容量：读 MR2 的 density[2:0]。
 * ⚠️ 只读 **8 位**。曾经读 16 位再 `>>5` 取密度 —— 那样低位是 MR2、高位是 MR3，
 *    把 16MB 读成 64MB，于是 MMU 映了 1024 页（超出物理容量，读回来全是垃圾）。 */
static uint32_t psram_size_bytes(void)
{
    uint8_t mr2 = 0;

    psram_xfer(AP_REG_READ, 16, 0x2, AP_RD_REG_DUMMY_BITLEN, 0, 0, &mr2, 8);
    switch (mr2 & 0x7u) {
    case 0x1: return 4u << 20;
    case 0x3: return 8u << 20;
    case 0x5: return 16u << 20;     /* 本板的 APS6408 = 16MB */
    case 0x7: return 32u << 20;
    case 0x6: return 64u << 20;
    default:  return 0;
    }
}

/* 把控制器从"单线 SPI"切成"DDR + 8 线 + AXI"。
 * ★ 这一步必须在"写 mode register + 连接性检查"**之后**（IDF 的顺序），
 *   因为前面的 mode register 访问靠的正是单线模式下的固定 dummy。 */
static void psram_config_mspi(void)
{
    psram_ctrlr_ll_set_wr_cmd(PSRAM_ID_CACHE, AP_WR_CMD_BITLEN, AP_SYNC_WRITE);
    psram_ctrlr_ll_set_rd_cmd(PSRAM_ID_CACHE, AP_RD_CMD_BITLEN, AP_SYNC_READ);

    psram_ctrlr_ll_set_addr_bitlen(PSRAM_ID_CACHE, AP_ADDR_BITLEN);
    psram_ctrlr_ll_enable_4byte_addr(PSRAM_ID_CACHE, true);

    psram_ctrlr_ll_set_wr_dummy(PSRAM_ID_CACHE, AP_WR_DUMMY_BITLEN);
    psram_ctrlr_ll_set_rd_dummy(PSRAM_ID_CACHE, AP_RD_DUMMY_BITLEN);
    psram_ctrlr_ll_enable_variable_dummy(PSRAM_ID_CACHE, true);
    psram_ctrlr_ll_enable_wr_dummy_level_control(PSRAM_ID_CACHE, true);

    psram_ctrlr_ll_enable_ddr_wr_data_swap(PSRAM_ID_CACHE, false);
    psram_ctrlr_ll_enable_ddr_rd_data_swap(PSRAM_ID_CACHE, false);
    psram_ctrlr_ll_enable_ddr_mode(PSRAM_ID_CACHE, true);

    psram_ctrlr_ll_enable_oct_line_mode(PSRAM_ID_CACHE, true);
    psram_ctrlr_ll_enable_hex_data_line_mode(PSRAM_ID_CACHE, false);

    psram_ctrlr_ll_enable_axi_access(PSRAM_ID_CACHE, true);      /* cache 侧走 AXI */
    psram_ctrlr_ll_enable_wr_splice(PSRAM_ID_CACHE, true);
    psram_ctrlr_ll_enable_rd_splice(PSRAM_ID_CACHE, true);
}

/*===========================================================================
 * 5. MMU 映射：0x50000000 起，1:1 映到 PSRAM 物理 0
 *
 * 页表项格式（soc/ext_mem_defs.h）：
 *     (paddr >> 16) | BIT(11) VALID | BIT(10) ACCESS_PSRAM
 * 这个格式有独立佐证：tmp/p4_nosdk（Chalandi 的裸机 P4 工程）的 mmu.c 用的就是它。
 *
 * ⚠️ **别用 ROM 的 Cache_PSRAM_MMU_Set**：它返回 0 但什么都不生效
 *    （ROM 头文件自己也写着 "Please do not call this function in your SDK application"），
 *    实测症状是 "rc=0 但一访问就 store access fault"，白查了一轮。
 *    这里用的 mmu_ll_* 就是 IDF mmu_hal_map_region() 的全部内容，纯寄存器、零分配。
 *===========================================================================*/
static int psram_mmu_map(uint32_t bytes)
{
    uint32_t vaddr = S31_PSRAM_VADDR;
    uint32_t pages = bytes / PSRAM_MMU_PAGE;
    uint32_t val, i, entry0, raw0;

    if (pages == 0) {
        return -1;
    }

    mmu_ll_set_page_size(MMU_LL_PSRAM_MMU_ID, MMU_PAGE_64KB);
    val = mmu_ll_format_paddr(MMU_LL_PSRAM_MMU_ID, 0, MMU_TARGET_PSRAM0);

    for (i = 0; i < pages; i++) {
        mmu_ll_write_entry(MMU_LL_PSRAM_MMU_ID,
                           mmu_ll_get_entry_id(MMU_LL_PSRAM_MMU_ID, vaddr),
                           val, MMU_TARGET_PSRAM0);
        vaddr += PSRAM_MMU_PAGE;
        val++;
    }

    /* 回读第 0 项自证：写进去就必须读得出来。ROM 那条路就是因为没有这一步，
     * "rc=0" 才骗了我们一整轮。
     * ⚠️ `SPI_MEM_S_MMU_ITEM_INDEX_REG` 本身读回来恒为 0（不回读），别拿它当证据。 */
    entry0 = mmu_ll_get_entry_id(MMU_LL_PSRAM_MMU_ID, S31_PSRAM_VADDR);
    raw0 = mmu_ll_read_entry(MMU_LL_PSRAM_MMU_ID, entry0);
    if (!(raw0 & SOC_MMU_PSRAM_VALID) || !(raw0 & SOC_MMU_ACCESS_PSRAM)) {
        rt_kprintf("[psram] MMU 页表项无效: %08x\r\n", (unsigned)raw0);
        return -1;
    }

    rt_kprintf("[psram] MMU: %u 页 @%08x -> PSRAM 物理 0（首项 %08x）\r\n",
            (unsigned)pages, (unsigned)S31_PSRAM_VADDR, (unsigned)raw0);
    return 0;
}

/*===========================================================================
 * 6. cache
 *===========================================================================*/

void s31_psram_invalidate_cache(uint32_t vaddr, uint32_t len)
{
    Cache_Invalidate_Addr(CACHE_MAP_ALL, vaddr, len);
}

/*===========================================================================
 * 7. 对外入口
 *===========================================================================*/
int s31_psram_init(void)
{
    uint32_t size;

    rt_kprintf("[psram] === 初始化（16MB 8 线 DDR @200MHz）===\r\n");

    /* ---- ⓪ PMA：把外部存储窗口的**写权限**打开 ----
     * ★ 这一步必须在任何 PSRAM 写之前，而且它是"cache 写 PSRAM 为什么全挂"的答案。
     *   ROM 把 [0x40000000, +512MB)（flash 映射窗口 + PSRAM 窗口）标成了只读，
     *   二级 bootloader 本来会调 esp_cpu_configure_region_protection() 把它打开，
     *   而本工程顶掉了二级 bootloader —— 所以只能自己补。详见上面的长注释。 */
    if (pma_grant_write(S31_PSRAM_VADDR) == 0) {
        rt_kprintf("[psram] 警告: 没找到覆盖 %08x 的 PMA 条目（写 PSRAM 可能会异常）\r\n",
                (unsigned)S31_PSRAM_VADDR);
    }

    /* ---- ① 供电与模拟前置 ---- */
    psram_ldo_enable();
    psram_ana_prereq();

    /* ---- ② MPLL 400MHz ---- */
    psram_mpll_init();

    /* ---- ③ 控制器：时钟/复位/选源 ---- */
    /* 用下划线版本，绕开 do{(void)__DECLARE_RCC_ATOMIC_ENV;...}while(0) 那层宏壳。
     * 裸机没有 RCC 锁，本来也不需要临界区包装。 */
    _psram_ctrlr_ll_enable_module_clock(PSRAM_ID_CACHE, true);
    _psram_ctrlr_ll_reset_module_clock(PSRAM_ID_CACHE);
    _psram_ctrlr_ll_select_clk_source(PSRAM_ID_CACHE, PSRAM_CLK_SRC_MPLL);
    _psram_ctrlr_ll_select_clk_source(PSRAM_ID_XFER,  PSRAM_CLK_SRC_MPLL);

    mspi_timing_ll_pin_drv_set(2);
    mspi_timing_ll_enable_dqs(true);

    psram_ctrlr_ll_set_cs_setup(PSRAM_ID_CACHE, AP_CS_SETUP_TIME);
    psram_ctrlr_ll_set_cs_hold(PSRAM_ID_CACHE, AP_CS_HOLD_TIME);
    psram_ctrlr_ll_set_cs_hold_delay(PSRAM_ID_CACHE, AP_CS_HOLD_DELAY);
    psram_ctrlr_ll_enable_split_trans(PSRAM_ID_CACHE, true);
    psram_ctrlr_ll_set_page_size(PSRAM_ID_CACHE, 2048);

    psram_ctrlr_ll_set_bus_clock(PSRAM_ID_CACHE, PSRAM_MPLL_FREQ_MHZ / PSRAM_SPEED_MHZ);
    psram_ctrlr_ll_set_bus_clock(PSRAM_ID_XFER,  PSRAM_MPLL_FREQ_MHZ / PSRAM_SPEED_MHZ);
    psram_ctrlr_ll_enable_dll(PSRAM_ID_CACHE, true);
    psram_ctrlr_ll_enable_dll(PSRAM_ID_XFER,  true);

    /* ---- ④ mode register + 连接性检查 ----
     * ⚠️ 顺序必须是"先写、再检查"（IDF 的顺序）。反过来（先检查、通过了才写）
     *    在冷启动时必然 MISMATCH -> 永远走不到写入 -> 永远 MISMATCH。 */
    psram_write_mode_reg();
    if (psram_check_connected() != 0) {
        rt_kprintf("[psram] 芯片没响应 -> 放弃（bootloader 继续跑，只是 App 不能放 PSRAM）\r\n");
        return -1;
    }

    size = psram_size_bytes();
    if (size == 0 || size > S31_PSRAM_VSIZE) {
        size = 16u << 20;
    }

    /* ---- ⑤ 切成 DDR + 8 线 + AXI ---- */
    psram_config_mspi();
    psram_ctrlr_ll_enable_variable_dummy(PSRAM_ID_CACHE, true);
    psram_ctrlr_ll_enable_variable_dummy(PSRAM_ID_XFER,  true);

    /* ---- ⑥ MMU 映射 ---- */
    if (psram_mmu_map(size) != 0) {
        return -1;
    }

    /* ---- ⑦ cache：时钟强制常开 + 使能 ----
     * IDF 的 cache_hal_init() 干的就是这两件事（它还有 cache_ll_l1_enable_bus()，
     * 但那些 SHUT 位本来就是 0）。裸机下没人替我们做，只能自己来。 */
    S31_REG32(R_HP_SYS_CLKRST_CACHE_CTRL0) |= (1u << 1) | (1u << 4) | (1u << 7) | (1u << 10);
    Cache_Enable_L1_DCache(0);            /* autoload = 0，与 IDF 默认一致 */
    Cache_Enable_L1_CORE0_ICache(0);


    /* ---- ⑧ 交叉验证：只有这一步能证明 PSRAM 真的可读可写 ----
     *   - 写走原始 MSPI 事务（唯一走得通的写路径）
     *   - 读分别用「原始事务」和「cache 映射窗口」各验一遍
     * 两条路都过 = "App 能搬进 PSRAM 并且读得到"。 */
    {
        static uint8_t wr[256], rd_raw[256];
        volatile uint8_t *rd_cch;
        uint32_t i, e_raw = 0, e_cch = 0;

        for (i = 0; i < sizeof(wr); i++) {
            wr[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
        }
        s31_psram_write_raw(0x1000u, wr, sizeof(wr));
        s31_psram_read_raw(0x1000u, rd_raw, sizeof(rd_raw));
        for (i = 0; i < sizeof(wr); i++) {
            if (rd_raw[i] != wr[i]) {
                e_raw++;
            }
        }

        /* 写完让 cache 失效，否则读到的可能是陈旧内容 */
        s31_psram_invalidate_cache(S31_PSRAM_VADDR + 0x1000u, sizeof(wr));
        rd_cch = (volatile uint8_t *)(S31_PSRAM_VADDR + 0x1000u);
        for (i = 0; i < sizeof(wr); i++) {
            if (rd_cch[i] != wr[i]) {
                e_cch++;
            }
        }

        s_psram_ok = (e_raw == 0 && e_cch == 0) ? 1 : 0;
        rt_kprintf("[psram] 自检: 原始写+原始读 %s / 原始写+cache读 %s -> %s\r\n",
                e_raw ? "FAIL" : "OK", e_cch ? "FAIL" : "OK",
                s_psram_ok ? "PSRAM 可用" : "PSRAM 不可用");
    }

    return s_psram_ok ? 0 : -1;
}

/* PSRAM 读写自检（4KB 太多余了，这里只测 base 起 256 字节）。
 * ★ 写必须走原始事务：普通 store 在本芯片上会 store access fault。
 *   这个组合（原始写 + cache 读）恰好就是 App 装载的真实路径，
 *   所以它通过 == "App 能搬进 PSRAM 并且读得到"。 */
uint32_t s31_psram_crc_test(uint32_t base, uint32_t bytes)
{
    static uint32_t buf[64];
    volatile uint32_t *p = (volatile uint32_t *)base;
    uint32_t n = bytes / 4u, i, errs = 0;

    if (base < S31_PSRAM_VADDR || n > 64u) {
        return 0xFFFFFFFFu;              /* 只支持 PSRAM 窗口里的前 256 字节 */
    }
    for (i = 0; i < n; i++) {
        buf[i] = 0xA5A50000u ^ (i * 0x01010101u);
    }
    /* 写走原始事务（普通 store 会 store access fault），地址用**物理地址**
       = 虚拟地址 - 0x50000000（本工程 MMU 是 1:1 映射）*/
    s31_psram_write_raw(base - S31_PSRAM_VADDR, buf, n * 4u);
    s31_psram_invalidate_cache(base, n * 4u);

    for (i = 0; i < n; i++) {
        uint32_t expect = 0xA5A50000u ^ (i * 0x01010101u);
        if (p[i] != expect) {
            errs++;
        }
    }
    return errs;
}

/*===========================================================================
 * 8. RT-Thread 侧的薄包装（移植时新加的，原文件没有）
 *===========================================================================*/

/* 初始化成功过没有。命令行拿它把关：没成功就别去碰 PSRAM 窗口
 * （PMA 没补上写权限时，一次普通 store 就是 store access fault）。*/
int s31_psram_ready(void)
{
    return s_psram_ok;
}

/* 芯片容量（字节）。init 失败时返回 0。*/
uint32_t s31_psram_size(void)
{
    uint32_t sz;

    if (!s_psram_ok) {
        return 0;
    }
    sz = psram_size_bytes();
    if (sz == 0 || sz > S31_PSRAM_VSIZE) {
        sz = 16u << 20;
    }
    return sz;
}
