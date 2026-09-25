/*===========================================================================
 * drv_clk.c -- 把 CPU 从 XTAL 40MHz 提到 CPLL 320MHz
 *
 * 为什么可行/为什么是我们自己做：ROM 交棒时 CPU 跑在 XTAL 40MHz（日志里那句
 * `SPI mode:DIO, clock div:1` 就是），IDF 的 320MHz 是**二级 bootloader（先到 80MHz）
 * + app（再切 320MHz）**两步配出来的。我们这个 port 没有 bootloader 也没有 app，
 * 所以要自己配。
 *
 * 寄存器（全部核对 IDF 6.1 的 soc/esp32s31 与 esp_hal_clock/esp32s31）：
 *   源选择   HP_SYS_CLKRST_SOC_CLK_SEL   = 0x20587000  bits[1:0]：0=XTAL 1=CPLL 2=RC_FAST 3=PLL_F240M
 *   分频     +0x04 CPU(div_num[7:0],值=分频-1) +0x08 MEM +0x0C SYS +0x10 APB
 *   生效     +0x14 ROOT_CLK_CTRL0.reg_soc_clk_update = 1（写完等自清；**原子生效点**）
 *   CPLL 上电 PMU_IMM_HP_CK_POWER_1 = 0x207040F4：bit27 XPD_CPLL、bit23 XPD_CPLL_I2C(WT)、
 *            bit19 GLOBAL_CPLL_ICG；HP_ALIVE_SYS_HP_CLK_CTRL = 0x20589000 bit29
 *   CPLL 倍频 LP_AONCLKRST_CPLL_DIV_REG = 0x2070104C：ref_div[3:0]、fb_div[11:4]
 *            → 40MHz × 8 / 1 = 320MHz
 *   校准     HP_SYS_CLKRST_ANA_PLL_CTRL0 = 0x20587174：bit3 CAL_STOP(1=停)、
 *            bit2 CAL_END(RO，校准完成)。先清 bit3 启动，等 bit2，10us 后置 bit3。
 *   自检     CPU_SRC_FREQ0 = 0x20587568（RO，步进 0.25MHz → 320MHz 读 1280）
 *            CPU_CLK_STATUS0 = 0x2058716C：bit2 CPU_SRC_IS_CPLL、bit1 CPU_DIV_EFFECT、
 *            [10:3] CPU_DIV_NUM_CUR
 *   ROM 收尾 ets_update_cpu_frequency = 0x2f800044（必须调，否则 ROM 的延时按 40MHz 算）
 *
 * ⚠️ CPLL 频率是纯数字域配的，**不需要 regi2c**（IDF 的 clk_ll_cpll_set_config 在
 *    S31 上就是空函数 + 断言）。
 * ⚠️ SYSTIMER 固定 16MHz（XTAL/2.5）、SPI2/3 与 I2C0/1 的源也都是 XTAL —— 升频不影响它们，
 *    所以 tick、我们的 I2C 时序计算、ROM 打印全都不用改。
 *    但 **APB 会从 6.67MHz 变成 53.33MHz**（POR 默认是 40/20/40÷3/40÷6），
 *    以 APB 为功能时钟的外设（如 PCNT）时序会变。
 *===========================================================================*/

#include <rtthread.h>
#include "s31_regs.h"

/* 目标主频（MHz）。默认 320，实测能起（见 README）；
 * 需要退回 POR 的 XTAL 档时不用改代码：`pwsh tools\build.ps1 -Safe`
 * 会用 -DS31_CLK_TARGET_MHZ=40 覆盖这里（40 = 完全不动时钟树）。*/
#ifndef S31_CLK_TARGET_MHZ
#define S31_CLK_TARGET_MHZ  320u
#endif

#define HP_SYS_CLKRST_BASE      0x20587000u
#define SOC_CLK_SEL_REG         (HP_SYS_CLKRST_BASE + 0x00u)
#define CPU_FREQ_CTRL0_REG      (HP_SYS_CLKRST_BASE + 0x04u)
#define MEM_FREQ_CTRL0_REG      (HP_SYS_CLKRST_BASE + 0x08u)
#define SYS_FREQ_CTRL0_REG      (HP_SYS_CLKRST_BASE + 0x0Cu)
#define APB_FREQ_CTRL0_REG      (HP_SYS_CLKRST_BASE + 0x10u)
#define ROOT_CLK_CTRL0_REG      (HP_SYS_CLKRST_BASE + 0x14u)
#define CPU_SRC_FREQ0_REG       (HP_SYS_CLKRST_BASE + 0x168u)   /* RO，0.25MHz/step */
#define CPU_CLK_STATUS0_REG     (HP_SYS_CLKRST_BASE + 0x16Cu)
#define ANA_PLL_CTRL0_REG       (HP_SYS_CLKRST_BASE + 0x174u)

#define PMU_HP_CK_POWER_1_REG   0x207040F4u
#define PMU_TIE_HIGH_XPD_CPLL        (1u << 27)
#define PMU_TIE_HIGH_XPD_CPLL_I2C    (1u << 23)
#define PMU_TIE_HIGH_GLOBAL_CPLL_ICG (1u << 19)
#define HP_ALIVE_SYS_CLK_CTRL_REG   0x20589000u
#define HP_CPLL_300M_CLK_EN         (1u << 29)

#define LP_AON_CPLL_DIV_REG     0x2070104Cu
#define CPLL_REF_DIV_S          0
#define CPLL_FB_DIV_S           4

#define ANA_PLL_CAL_STOP        (1u << 3)
#define ANA_PLL_CAL_END         (1u << 2)

#define CLK_STATUS_SRC_IS_CPLL  (1u << 2)
#define CLK_STATUS_DIV_EFFECT   (1u << 1)

#define CPU_FREQ_CTRL_DIV_M     0x3FFu          /* div_num[7:0] + num[10:8] + den[13:11] */
#define MEM_FREQ_CTRL_DIV_M     0x1u
#define SYS_FREQ_CTRL_DIV_M     0xFFu
#define APB_FREQ_CTRL_DIV_M     0xFFu

#define ROM_ETS_UPDATE_CPU_FREQ ((void (*)(rt_uint32_t))0x2f800044u)

void s31_usj_tx_pump(void);
rt_uint32_t s31_usj_tx_pending(void);
rt_uint32_t s31_clk_measured_mhz(void);      /* 定义在后面，前面要用 */

/* 打点并**尽量**等它真的发出去：开机阶段 tick 还没起来，2KB 发送环形缓冲没人抽，
 * 直接 rt_kprintf 的话后面的标记会卡在环里（切频失败就再也看不到 → 定位不了）。
 * 🚨 但**绝不能无限等**：没插终端（或者主机没在读）时环永远排不空 →
 *    固件卡死在开机第一步，"板子看起来是死的"（实测踩过：复现后连 ROM 横幅都看不到，
 *    只能断电；也解释了为什么"上电但没开串口"的板子会像砖一样）。
 *    guard 现在只够"正常有终端时把字吐出去"，等不到就算了。*/
static void clk_mark(const char *s)
{
    volatile rt_uint32_t guard = 200000u;

    rt_kprintf("%s", s);
    while (s31_usj_tx_pending() && --guard) {
        s31_usj_tx_pump();
    }
}

/* 读 CPU 源频率（硬件频率表，不用自己算周期） */
rt_uint32_t s31_clk_cpu_mhz_hw(void)
{
    return S31_REG32(CPU_SRC_FREQ0_REG) / 4u;      /* 0.25MHz/step → MHz */
}

rt_uint32_t s31_clk_status(void)
{
    return S31_REG32(CPU_CLK_STATUS0_REG);
}

/* 当前 CPU 是否已经跑在 CPLL 上（IDF 启动链下会是 true） */
int s31_clk_on_cpll(void)
{
    return (S31_REG32(CPU_CLK_STATUS0_REG) & CLK_STATUS_SRC_IS_CPLL) ? 1 : 0;
}

static void s31_clk_set_dividers(rt_uint32_t cpu_div, rt_uint32_t mem_div,
                                 rt_uint32_t sys_div, rt_uint32_t apb_div)
{
    S31_REG32(CPU_FREQ_CTRL0_REG) =
        (S31_REG32(CPU_FREQ_CTRL0_REG) & ~CPU_FREQ_CTRL_DIV_M) | ((cpu_div - 1u) & 0xFFu);
    S31_REG32(MEM_FREQ_CTRL0_REG) =
        (S31_REG32(MEM_FREQ_CTRL0_REG) & ~MEM_FREQ_CTRL_DIV_M) | ((mem_div - 1u) & 0x1u);
    S31_REG32(SYS_FREQ_CTRL0_REG) =
        (S31_REG32(SYS_FREQ_CTRL0_REG) & ~SYS_FREQ_CTRL_DIV_M) | ((sys_div - 1u) & 0xFFu);
    S31_REG32(APB_FREQ_CTRL0_REG) =
        (S31_REG32(APB_FREQ_CTRL0_REG) & ~APB_FREQ_CTRL_DIV_M) | ((apb_div - 1u) & 0xFFu);
}

/* 切源 + 原子生效（带超时：万一 update 位不自清，也不能把系统挂死在这里） */
static int s31_clk_switch_src(rt_uint32_t sel)
{
    volatile rt_uint32_t guard = 200000u;

    S31_REG32(SOC_CLK_SEL_REG) = (S31_REG32(SOC_CLK_SEL_REG) & ~0x3u) | (sel & 0x3u);
    S31_REG32(ROOT_CLK_CTRL0_REG) = 1u;            /* reg_soc_clk_update */
    while ((S31_REG32(ROOT_CLK_CTRL0_REG) & 1u) && --guard) {
    }
    if (guard == 0) {
        rt_kprintf("[clk] !! update bit stuck (root=%08x sel=%08x)\n",
                   (unsigned)S31_REG32(ROOT_CLK_CTRL0_REG),
                   (unsigned)S31_REG32(SOC_CLK_SEL_REG));
        return -1;
    }
    return 0;
}

/* CPLL 上电 + 配成 320MHz + 自校准；失败返回 -1
 * 顺序照 IDF 的 rtc_clk_cpll_configure()（rtc_clk.c:138-156）：
 *   set fb/ref → ANALOG_CLOCK_ENABLE → CAL start → 等 CAL_END → 10us → CAL stop → ANALOG_CLOCK_DISABLE
 * ⚠️ 调试期每步打一个短标记（<64B，能立刻出 TX FIFO）：一旦某步之后卡死，
 *    日志就停在那一步 —— 这是唯一能定位的手段（切频失败会直接没输出）。 */
static int s31_cpll_setup_320m(void)
{
    volatile rt_uint32_t guard;
    rt_uint32_t fb_ref;

    clk_mark("[clk] 1 modem/rx clk\n");
    /* 0. 前置：MODEM 寄存器总线时钟 + regi2c 主时钟（IDF 的 ANALOG_CLOCK_ENABLE）*/
    S31_REG32(0x20587040u) |= (1u << 0);            /* HP_SYS_CLKRST_MODEM_CTRL0.reg_modem_clk_en */
    S31_REG32(0x2010F018u) |= (1u << 2);            /* MODEM_LPCON.clk_conf.clk_i2c_mst_en */

    clk_mark("[clk] 2 cpll power\n");
    /* 1. CPLL 上电（顺序照 IDF：XPD → XPD_I2C → GLOBAL_ICG） */
    S31_REG32(PMU_HP_CK_POWER_1_REG) |= PMU_TIE_HIGH_XPD_CPLL;
    S31_REG32(PMU_HP_CK_POWER_1_REG) |= PMU_TIE_HIGH_XPD_CPLL_I2C;
    S31_REG32(PMU_HP_CK_POWER_1_REG) |= PMU_TIE_HIGH_GLOBAL_CPLL_ICG;
    S31_REG32(HP_ALIVE_SYS_CLK_CTRL_REG) |= HP_CPLL_300M_CLK_EN;

    clk_mark("[clk] 3 fb/ref=8/1\n");
    /* 2. 倍频：40MHz × 8 / 1 = 320MHz（ref_div=1, fb_div=8） */
    fb_ref = S31_REG32(LP_AON_CPLL_DIV_REG);
    fb_ref &= ~((0xFu << CPLL_REF_DIV_S) | (0xFFu << CPLL_FB_DIV_S));
    fb_ref |= (1u << CPLL_REF_DIV_S) | (8u << CPLL_FB_DIV_S);
    S31_REG32(LP_AON_CPLL_DIV_REG) = fb_ref;

    clk_mark("[clk] 4 cal start\n");
    /* 3. 自校准：清 CAL_STOP 启动 → 等 CAL_END → 停 */
    S31_REG32(ANA_PLL_CTRL0_REG) &= ~ANA_PLL_CAL_STOP;
    guard = 200000u;
    while (!(S31_REG32(ANA_PLL_CTRL0_REG) & ANA_PLL_CAL_END) && --guard) {
    }
    if (guard == 0) {
        rt_kprintf("[clk] cal TIMEOUT (ana=%08x)\n", (unsigned)S31_REG32(ANA_PLL_CTRL0_REG));
        return -1;                                  /* 校准超时 → 别切源 */
    }
    for (guard = 0; guard < 400; guard++) {         /* ~10us 忙等（照 IDF） */
    }
    S31_REG32(ANA_PLL_CTRL0_REG) |= ANA_PLL_CAL_STOP;
    rt_kprintf("[clk] 5 cal done (ana=%08x)\n", (unsigned)S31_REG32(ANA_PLL_CTRL0_REG));
    return 0;
}

/* 降到 XTAL 40MHz（兜底；不禁 PLL） */
void s31_clk_to_xtal_40m(void)
{
    s31_clk_set_dividers(1, 1, 1, 1);
    s31_clk_switch_src(0);                          /* 0 = XTAL */
    ROM_ETS_UPDATE_CPU_FREQ(40);
}

/* 主入口：40MHz → target MHz（CPLL 固定 320MHz，靠 CPU 分频器分档）
 *
 * ✅ 320MHz **实测可用**（rdcycle×systimer 量到 319~320MHz，`s31_spin` 循环计数 40MHz 档的 8.5 倍）。
 *    注意：**没有移植 IDF 的 `pmu_init()`（数字稳压器配置）**，长期稳定性未验证；
 *    但"每 3 秒复位一次"那个现象与时钟无关，真凶是超级看门狗（见 `startup.S` 的说明）。
 *
 * 分频表（IDF rtc_clk.c:223-280 的注释 + 约束 MEM≤160M、SYS≤320/3、APB≤320/6）：
 *   320MHz: cpu÷1 mem÷2(160M) sys÷3(106.7M) apb÷2(53.3M)
 *   160MHz: cpu÷2 mem÷1(160M) sys÷2(80M)    apb÷2(40M)
 *    80MHz: cpu÷4 mem÷1(80M)  sys÷1(80M)    apb÷2(40M)
 */
int s31_cpu_clk_to_mhz(rt_uint32_t target_mhz)
{
    rt_uint32_t cpu_div, mem_div, sys_div, apb_div;

    switch (target_mhz) {
    case 320: cpu_div = 1; mem_div = 2; sys_div = 3; apb_div = 2; break;
    case 160: cpu_div = 2; mem_div = 1; sys_div = 2; apb_div = 2; break;
    case 80:  cpu_div = 4; mem_div = 1; sys_div = 1; apb_div = 2; break;
    default:  return -1;
    }

    clk_mark("[clk] src check\n");
    rt_kprintf("[clk] src=%08x status=%08x target=%uMHz\n",
               (unsigned)S31_REG32(SOC_CLK_SEL_REG),
               (unsigned)s31_clk_status(), (unsigned)target_mhz);

    if (s31_clk_on_cpll()) {
        ROM_ETS_UPDATE_CPU_FREQ(target_mhz);
        return 0;                                   /* 已经切过（比如 IDF bootloader 配好的）*/
    }

    if (s31_cpll_setup_320m() != 0) {
        s31_clk_to_xtal_40m();
        return -1;
    }

    s31_clk_set_dividers(cpu_div, mem_div, sys_div, apb_div);
    clk_mark("[clk] 6 dividers set\n");
    if (s31_clk_switch_src(1) != 0) {               /* 1 = CPLL */
        return -1;
    }
    clk_mark("[clk] 7 switched\n");
    ROM_ETS_UPDATE_CPU_FREQ(target_mhz);

    return 0;
}

int s31_cpu_clk_to_320m(void) { return s31_cpu_clk_to_mhz(S31_CLK_TARGET_MHZ); }

/* 用 rdcycle × SYSTIMER（固定 16MHz）交叉测真实主频 —— 这是我们的**基准口径**
 * （那个"硬件频率表"寄存器 0x20587168 在 XTAL 档下读出 75MHz，明显不是 CPU 频率，
 *  所以只当参考、不当依据）。测量法不依赖任何时钟假设。 */
rt_uint32_t s31_clk_measured_mhz(void)
{
    extern rt_uint64_t s31_systimer_get_ticks(void);
    rt_uint32_t c0, c1;
    rt_uint64_t t0, t1;
    rt_uint32_t i;

    __asm__ volatile ("rdcycle %0" : "=r"(c0));
    t0 = s31_systimer_get_ticks();
    for (i = 0; i < 200000u; i++) {                 /* 空循环占点时间 */
        __asm__ volatile ("nop");
    }
    __asm__ volatile ("rdcycle %0" : "=r"(c1));
    t1 = s31_systimer_get_ticks();

    if (t1 == t0) {
        return 0;
    }
    /* cycles / (systimer差值/16) = MHz */
    return (rt_uint32_t)(((rt_uint64_t)(c1 - c0) * 16u) / (t1 - t0));
}

/* 只做切频，**不测量**：测量要读 SYSTIMER，而 SYSTIMER 的时钟门控是
 * s31_systick_hw_init() 才打开的 —— 在它之前读会死等 VALUE_VALID
 * （踩过：表现为"切频之后就没输出了"，其实跟频率无关）。 */

/* ---- 提频守卫（失败自动兜底）--------------------------------------------
 * 问题：时钟切错了会**复位循环/卡死**，那时串口什么都没有，只能改代码重烧。
 * 做法：把哨兵放在链接脚本的 `.noinit` 段（**startup.S 不清它**，bootROM 也不清 SRAM，
 *      只有断电才没）：
 *   ① 切频前写哨兵；
 *   ② 系统真正跑起来 200ms（main 线程里）再清掉；
 *   ③ 下次启动若发现哨兵还在 → 上一轮"切完没跑到确认"（多半挂了）
 *      → **这次不提频**，保持 ROM 交棒的 XTAL 档，并把跳过次数 +1。
 * 提频把板子搞挂时能自己恢复，不用碰硬件、不用重烧。
 *
 * ⚠️ 别用 LP 域的 scratch 寄存器做这件事：`LP_SYSTEM_REG_LP_STORE0/1`
 *    实测（2026-09-23）在 `rst:0x17` 这类复位后**不保留**（手动写进去的哨兵,
 *    复位后读回 0），而且 STORE1 里本来就有 ROM 放的数（读出来 0x0036321a），
 *    乱写它等于踩别人的寄存器。SRAM 里的 .noinit 才靠得住。*/
#define S31_CLK_GUARD_MAGIC 0x53333143u     /* "S31C" */

static volatile rt_uint32_t s_clk_guard __attribute__((section(".noinit")));
static volatile rt_uint32_t s_clk_guard_skips __attribute__((section(".noinit")));
static volatile rt_uint32_t s_clk_guard_skips_inv __attribute__((section(".noinit")));
static rt_uint32_t s_clk_guard_armed;

/* 跳过次数要**自校验**：`.noinit` 故意不清零，刚烧完新固件时这块 RAM 里
 * 是上一版镜像留下的东西 —— 直接当计数读出来就会打印 3684562551 这种天文数字
 * （用户实测遇到过；虽然只影响显示、不影响提频判断，但很误导人）。
 * 办法：计数 + 它的反码成对写，两者不互补就认为"没初始化过" → 归零。*/
static rt_uint32_t clk_guard_skips_get(void)
{
    rt_uint32_t n = s_clk_guard_skips;

    if (n != ~s_clk_guard_skips_inv) {
        n = 0;
        s_clk_guard_skips = 0;
        s_clk_guard_skips_inv = ~0u;
    }
    return n;
}

static void clk_guard_skips_inc(void)
{
    rt_uint32_t n = clk_guard_skips_get() + 1u;

    s_clk_guard_skips = n;
    s_clk_guard_skips_inv = ~n;
}

int s31_clk_init(void)
{
#if S31_CLK_TARGET_MHZ <= 40u
    /* -Safe 档（`tools\build.ps1 -Safe`）：压根不动时钟树，守卫不用武装 */
    rt_kprintf("[clk] safe 档：保持 ROM 交棒的 XTAL 40MHz，不动 PLL\n");
    return 0;
#else
    if (s_clk_guard == S31_CLK_GUARD_MAGIC) {
        s_clk_guard = 0;
        clk_guard_skips_inc();
        rt_kprintf("[clk] 守卫：上一轮提频后没跑到确认（累计 %u 次）"
                   "→ 本次**主动切回 XTAL 40MHz**（哨兵@%p）\n",
                   (unsigned)clk_guard_skips_get(), (void *)&s_clk_guard);
        /* 🚨 光"什么都不做"不够：`rst:0x17` 这类复位**不会**把 CPLL/根时钟复位掉，
         *    上次留下的 320MHz 配置还在（实测守卫跳过时量出来仍是 320MHz）。
         *    所以这里显式切回 XTAL，保证真的回到安全档。*/
        s31_clk_to_xtal_40m();
        return 0;
    }

    s_clk_guard = S31_CLK_GUARD_MAGIC;
    s_clk_guard_armed = 1;
    return s31_cpu_clk_to_320m();
#endif
}

/* 系统跑起来之后（main 线程里）确认成功、撤掉哨兵 */
void s31_clk_guard_clear(void)
{
    if (s_clk_guard_armed) {
        s_clk_guard_armed = 0;
        s_clk_guard = 0;
    }
}

/* 给 s31_info / s31_clk_cmd 看的守卫状态 */
rt_uint32_t s31_clk_guard_skips(void)
{
    return clk_guard_skips_get();       /* 带自校验：没初始化过就是 0 */
}

/* 哨兵变量地址（msh 里可以 s31_reg w <addr> <magic> 手动复现"上次挂了"）*/
rt_uint32_t s31_clk_guard_addr(void)
{
    return (rt_uint32_t)(rt_ubase_t)&s_clk_guard;
}

/* 频率自检（要在 SYSTIMER 起来之后调）：rdcycle × systimer 交叉测量。
 * 这是我们的基准口径 —— 那个"硬件频率表"寄存器(0x20587168)在 XTAL 档下就读出
 * 75MHz，明显不是 CPU 频率，不能当依据。 */
void s31_clk_report(void)
{
    rt_kprintf("[clk] CPU = %u MHz by rdcycle x systimer (target %u) status=%08x",
               (unsigned)s31_clk_measured_mhz(), (unsigned)S31_CLK_TARGET_MHZ,
               (unsigned)s31_clk_status());
    if (s_clk_guard_armed) {
        rt_kprintf(" guard=ARMED");
    }
    if (s31_clk_guard_skips()) {
        rt_kprintf(" guard_skips=%u", (unsigned)s31_clk_guard_skips());
    }
    rt_kprintf("\n");
}

/* 🚨 不能用 INIT_BOARD_EXPORT：本 port 没有在 rt_hw_board_init() 里调
 * rt_components_board_init()，1 级的初始化函数永远不会执行（AGENTS.md §4.18）。
 * 这里由 rt_hw_board_init() 显式调用（要赶在开机横幅和 tick 之前）。 */
