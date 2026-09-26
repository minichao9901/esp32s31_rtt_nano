/*===========================================================================
 * sfud_port.c -- SFUD 移植层（ESP32-S31 / RT-Thread Nano，走 drv_spi.c 的 spi2）
 *
 * SFUD 要用户填的东西只有 `sfud_spi` 里的 4 个钩子（见 sfud_def.h:247）：
 *     wr()      一次全双工事务：先发 write_size 字节，再发 read_size 个字节收回来
 *     lock()/unlock()  互斥锁（S31 是单核，但 msh 线程和别的线程可能同时用总线）
 *     user_data 指回用户的上下文
 * 外加 `sfud_spi_port_init()`（SFUD 在 hardware_init 里回调，用来填上面这些）。
 *
 * ---- 本 port 的两个要点 ----
 *
 * ① 🚨 **必须切块**：`drv_spi.c` 是 CPU 轮询（PIO）通路，一次最多 64 字节
 *    （16 字 FIFO）。超了驱动 `rt_set_errno(-RT_EINVAL)` 并返回 0 ——
 *    不报错不崩，只是"这次传输什么都没发生"，读回来的还是缓冲区里的旧值。
 *    所以 wr() 要把 "命令+地址" 和 "数据" 分开、并把数据切成 ≤64 字节的若干次
 *    `rt_spi_send_then_recv()`。中间各段之间 CS 一直是低的：驱动在
 *    `cs_take=1` 的 message 上置 `SPI_MISC.cs_keep_active`，最后一段才放。
 *
 * ② 🚨 **命令+地址必须一次发完**（不能和第一段数据合并成一次 transfer）：
 *    flash 收到 0x03/0x02 + 3 字节地址就进数据相位了，此后靠时钟继续推进。
 *    这里发完命令+地址**抬起 MOSI 数据**的那次 transfer 结束时会有一个空档
 *    （软件重配寄存器），但 CS 没抬、时钟没走 → 对从机来说时间静止，
 *    所以地址后面直接跟数据是合法的。**这一点和"CS 抬起来"完全不是一回事**，
 *    别把两者混为一谈。
 *
 * ---- 与 RT-Thread 官方移植层（dev_spi_flash_sfud.c）的差别 ----
 *   · 那份要 `RT_USING_SFUD` + 建块设备（rt_device_register）/DFS 那一套；
 *     本工程是 Nano 风格、没有 DFS，用不上块设备，所以只留 SFUD 引擎 + 这层胶水。
 *   · 那份的 lock 用 `struct spi_flash_device` 里的锁；这里自己建一把。
 *   · 那份靠 Kconfig 传配置；这里 `s31_sfud_probe()` 三个参数传进来。
 *
 * 接线（与 drv_spi.c 的默认档、也是本工作区 spi_flash_sfud 工程的接法一致）：
 *   SCK=GPIO43(J2-17)  MOSI=GPIO44(J2-18)  MISO=GPIO45(J2-15)  CS=GPIO46(J2-16)
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdarg.h>

#include "sfud.h"
#include "sfud_port.h"
#include "s31_regs.h"           /* S31_SYSTIMER_HZ */

extern rt_uint64_t s31_systimer_get_ticks(void);        /* drv_systick.c */

/* 打开它：每次 SPI 事务都打一行 tx/rx（只用于排"收到的字节不对"）。
 * 平时关掉 —— 一次 probe 上百行。*/
#define S31_SFUD_TRACE_XFER     0

/*---------------------------------------------------------------------------
 * SFUD 的调试日志出口（只在 sfud_cfg.h 打开 SFUD_DEBUG_MODE 时才被引用）。
 *
 * 打开后每条命令都会打一行 —— 排"读回来的字节不对"这类问题时，
 * 有它才能看到 SFUD 到底发了什么、收到的又是什么。
 * 平时**关着**：一次 probe 就上百行，真机日志会被刷爆。
 *-------------------------------------------------------------------------*/
void sfud_log_debug(const char *file, const long line, const char *format, ...)
{
    /* ⚠️ 缓冲用 **static** 而不是栈上的：这条路径挂在 tshell 线程上，
     *    而 tshell 只有 FINSH_THREAD_STACK_SIZE（4096）字节，
     *    sfud_device_init() 里已经堆了 sfud_flash（~200B）+ 各层局部量，
     *    这里再吃 128B 栈很容易在排错时把栈踩了 —— 那会变成
     *    "打开调试日志之后行为就变了"这种最难查的问题。*/
    static char buf[128];
    va_list args;

    va_start(args, format);
    rt_vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    /* 只打文件名，别把整条路径铺上去（日志短一半）*/
    {
        const char *base = file;
        const char *p;
        for (p = file; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        rt_kprintf("[sfud-dbg] %s:%ld %s\n", base, line, buf);
    }
}

/*---------------------------------------------------------------------------
 * 绑定上下文（只有一份：本 port 一次挂一块外接 flash）
 *
 * 🚨 设备**本体**在 SFUD 自己的静态表里（sfud.c:40 的 `flash_table[]`），
 *    由 `SFUD_FLASH_DEVICE_TABLE`（sfud_cfg.h）决定 —— 本 port 是 1 个。
 *    为什么不自己再定义一份：`sfud_get_device(0)` 返回的是**表里那份**，
 *    如果 `sfud_device_init()` 初始化的是我们自己的副本，那么
 *    "初始化成功"和"get_device 拿到的设备"就是两个不同的对象 ——
 *    现象是 probe 明明成功，`sfud_get_device(0)->init_ok` 却永远是 0
 *    （本 port 第一版就是这么错的）。所以这里直接拿表里的那份去初始化。
 *-------------------------------------------------------------------------*/
static char        s_sfud_name[RT_NAME_MAX];
static struct rt_spi_device *s_sfud_spi;   /* 借用的 RT-Thread SPI 设备 */
static struct rt_mutex      s_sfud_lock;
static rt_bool_t            s_sfud_lock_ready = RT_FALSE;
static rt_uint32_t          s_sfud_hz;

/* 一次交给驱动的最大字节数：
 *   **0 = 不限制（默认）** —— 整段直接交给 `drv_spi.c`，由它自己决定用 PIO（≤64）
 *   还是 DMA（>64）。这才是"一次搬任意长度"该有的样子。
 *   非 0 = 强制按这个值切块，`sf bench` 用它量"每次事务的固定开销"
 *   （8/64 这两档走 PIO、512/4096 走 DMA，拐点一眼就能看出来）。*/
static rt_uint32_t s_sfud_max_xfer = 0u;

void s31_sfud_set_chunk(rt_uint32_t n)
{
    s_sfud_max_xfer = n;            /* 0 = 不切块 */
}

rt_uint32_t s31_sfud_get_chunk(void)
{
    return s_sfud_max_xfer;
}

/* 定长微秒忙等：SFUD 的重试延时就 100us 量级，用 tick（1ms）会慢 10 倍 */
static void sfud_delay_us(rt_uint32_t us)
{
    rt_uint64_t t0 = s31_systimer_get_ticks();
    rt_uint64_t n  = (rt_uint64_t)us * (S31_SYSTIMER_HZ / 1000000u);

    while ((s31_systimer_get_ticks() - t0) < n) {
    }
}

static void sfud_retry_delay_100us(void)
{
    sfud_delay_us(100u);
}

/*---------------------------------------------------------------------------
 * 锁
 *-------------------------------------------------------------------------*/
static void sfud_spi_lock(const sfud_spi *spi)
{
    (void)spi;
    if (s_sfud_lock_ready) {
        rt_mutex_take(&s_sfud_lock, RT_WAITING_FOREVER);
    }
}

static void sfud_spi_unlock(const sfud_spi *spi)
{
    (void)spi;
    if (s_sfud_lock_ready) {
        rt_mutex_release(&s_sfud_lock);
    }
}

/*---------------------------------------------------------------------------
 * ① 写：命令/地址 ≤ S31_SFUD_CMD_MAX，数据切成 ≤ S31_SFUD_DATA_MAX
 *    （写命令 0x02 一次数据相位最多 256 字节，而我们 64 就断了，天然满足）
 *-------------------------------------------------------------------------*/
static sfud_err sfud_spi_write(const uint8_t *write_buf, size_t write_size)
{
    struct rt_spi_device *dev = s_sfud_spi;

    if (dev == RT_NULL || write_buf == RT_NULL || write_size == 0u ||
        write_size > S31_SFUD_CMD_MAX) {
        return SFUD_ERR_WRITE;
    }
    /* ⚠️ 用 rt_spi_transfer 而不是 rt_spi_send：后者是 `rt_inline rt_size_t`
     * （**无符号**，dev_spi.h:576），驱动出错时返回的 -RT_EINVAL 会被它转成
     * 一个巨大的正数，`<= 0` 那种判错就永远不成立了。*/
    if (rt_spi_transfer(dev, write_buf, RT_NULL, write_size) <= 0) {
        return SFUD_ERR_WRITE;
    }
    return SFUD_SUCCESS;
}

/*---------------------------------------------------------------------------
 * ② 读：命令[+地址] 一段，数据一段
 *
 * ✅ **默认整段一次交给驱动**（`s31_sfud_max_xfer = 0`）：drv_spi.c 现在
 *    ≤64 字节走 PIO、>64 字节自动走 DMA，所以 4 KB 读就是**一次 SPI 事务**，
 *    不用再像以前那样自己按 64 字节切。
 *    （这是 2026-09-25 给 drv_spi.c 补上 DMA 之后改的：之前 PIO 一次只有 64 字节，
 *      移植层不得不切块，每次都要重发命令+地址、重配寄存器，只有线速的 64%。）
 *
 * ⚠️ 只有需要**切块**时（`sf bench` 把 max_xfer 调小）才要处理地址：
 *    切块读**必须每块递增命令里的地址**（flash 的读命令不自增）。
 *    且只有**普通读 0x03** 能这么算：0x9F 没有地址、0x5A(SFDP) 尾部还有空转字节。
 *-------------------------------------------------------------------------*/
static sfud_err sfud_spi_read(const uint8_t *write_buf, size_t write_size,
                              uint8_t *read_buf, size_t read_size)
{
    struct rt_spi_device *dev = s_sfud_spi;
    uint8_t cmd[S31_SFUD_CMD_MAX];
    rt_uint32_t base_addr = 0;
    size_t addr_len, i, off = 0;
    rt_bool_t adv;
    size_t chunk;

    if (dev == RT_NULL || read_buf == RT_NULL || read_size == 0u ||
        write_buf == RT_NULL || write_size == 0u ||
        write_size > S31_SFUD_CMD_MAX) {
        return SFUD_ERR_READ;
    }

    chunk = (s_sfud_max_xfer != 0u) ? (size_t)s_sfud_max_xfer : read_size;

    rt_memcpy(cmd, write_buf, write_size);
    addr_len = (write_size > 1u) ? (write_size - 1u) : 0u;
    adv = (write_buf[0] == SFUD_CMD_READ_DATA && (addr_len == 3u || addr_len == 4u))
              ? RT_TRUE : RT_FALSE;
    if (adv) {
        for (i = 0; i < addr_len; i++) {
            base_addr = (base_addr << 8) | write_buf[1u + i];
        }
    }

    while (off < read_size) {
        size_t n = read_size - off;

        if (n > chunk) {
            n = chunk;
        }
        if (adv && off != 0u) {                 /* 第 1 块用原地址，后面按块递增 */
            rt_uint32_t a = base_addr + (rt_uint32_t)off;
            for (i = 0; i < addr_len; i++) {
                cmd[1u + i] = (uint8_t)(a >> (8u * (addr_len - 1u - i)));
            }
        }
        if (rt_spi_send_then_recv(dev, cmd, write_size, read_buf + off, n) != RT_EOK) {
            return SFUD_ERR_READ;
        }
        off += n;
    }
    return SFUD_SUCCESS;
}

/*---------------------------------------------------------------------------
 * ③ 长写（页编程）：命令+地址 一段，数据一段（默认**整段一次交给驱动**）
 *
 * 页编程是"0x02 + 3 字节地址 + N 字节数据"这**一条连续片选**：中间不能抬 CS
 * （抬了从机就把这条命令丢了），所以用一条 message 链：
 *   第 1 条：命令+地址，cs_take=1、cs_release=0
 *   最后一条：数据（≤256 字节，一次交给驱动），cs_take=0、cs_release=1
 * ⚠️ 别用 `rt_spi_transfer()`：它把 cs_take/cs_release 都置 1，等于第一段发完就抬 CS。
 * ⚠️ 数据段默认发一整段（256 字节 > 64 ⇒ 驱动自动走 DMA）；
 *    只有 `sf bench` 把 max_xfer 调小时才切块。
 *-------------------------------------------------------------------------*/
#define S31_SFUD_WR_MAX_CHUNKS   ((SFUD_WRITE_MAX_PAGE_SIZE + 7u) / 8u + 1u)
static struct rt_spi_message s_wr_msgs[S31_SFUD_WR_MAX_CHUNKS];

static sfud_err sfud_spi_write_stream(const sfud_flash *flash,
                                      const uint8_t *write_buf, size_t write_size)
{
    struct rt_spi_device *dev = s_sfud_spi;
    size_t cmd_size = (flash != RT_NULL && flash->addr_in_4_byte) ? 5u : 4u;
    size_t data_len, off = 0;
    size_t chunk = (s_sfud_max_xfer != 0u) ? (size_t)s_sfud_max_xfer : SFUD_WRITE_MAX_PAGE_SIZE;
    rt_uint32_t n = 0;

    if (dev == RT_NULL || write_buf == RT_NULL || write_size <= cmd_size) {
        return SFUD_ERR_WRITE;
    }
    data_len = write_size - cmd_size;

    /* 第 1 段：命令 + 地址（3 或 4 字节地址，所以总共 4 或 5 字节 ≤ 8）*/
    rt_memset(&s_wr_msgs[n], 0, sizeof(s_wr_msgs[n]));
    s_wr_msgs[n].send_buf   = write_buf;
    s_wr_msgs[n].length     = cmd_size;
    s_wr_msgs[n].cs_take    = 1;
    s_wr_msgs[n].cs_release = 0;
    s_wr_msgs[n].next       = &s_wr_msgs[n + 1u];
    n++;

    /* 后面：数据；最后一段才放 CS */
    while (off < data_len) {
        size_t len = data_len - off;
        rt_bool_t last;

        if (len > chunk) {
            len = chunk;
        }
        last = (off + len >= data_len) ? RT_TRUE : RT_FALSE;

        /* ⚠️ 越界检查必须在**用之前**：写在 n++ 之后的话，最后一块正好把 n
         *    顶到数组上限，就会被误判成"链太长"而报错（本 port 踩过）。*/
        if (n >= S31_SFUD_WR_MAX_CHUNKS) {
            return SFUD_ERR_WRITE;
        }
        rt_memset(&s_wr_msgs[n], 0, sizeof(s_wr_msgs[n]));
        s_wr_msgs[n].send_buf   = write_buf + cmd_size + off;
        s_wr_msgs[n].length     = len;
        s_wr_msgs[n].cs_take    = 0;
        s_wr_msgs[n].cs_release = last;
        s_wr_msgs[n].next       = last ? RT_NULL : &s_wr_msgs[n + 1u];
        n++;
        off += len;
    }

    /* 链式发送（rt_spi_transfer_message 会自己遍历 next 逐个 xfer）*/
    rt_set_errno(RT_EOK);
    (void)rt_spi_transfer_message(dev, &s_wr_msgs[0]);
    return (rt_get_errno() == RT_EOK) ? SFUD_SUCCESS : SFUD_ERR_WRITE;
}

/* SFUD 的主钩子：一段写 + 一段读 */
static sfud_err sfud_spi_wr(const struct __sfud_spi *spi,
                            const uint8_t *write_buf, size_t write_size,
                            uint8_t *read_buf, size_t read_size)
{
    sfud_err e;

    if (write_size == 0u) {
        return SFUD_ERR_READ;
    }
    if (read_size != 0u) {
        e = sfud_spi_read(write_buf, write_size, read_buf, read_size);
    } else if (write_size <= S31_SFUD_CMD_MAX) {
        e = sfud_spi_write(write_buf, write_size);      /* WREN / 擦除 / 写状态 */
    } else {
        /* 🚨 页编程走这条：0x02 + 地址 + 最多 256 字节数据，一次 write_size 能到 260 */
        e = sfud_spi_write_stream((const sfud_flash *)spi->user_data,
                                  write_buf, write_size);
    }

    /* 排错用：把每次事务的"发什么/收什么"打出来。
     * ⚠️ 判据必须是 `#if` 而不是 `#ifdef` —— 宏定义成 0 也算"已定义"，
     *    用 #ifdef 的话这个开关**关不掉**（本 port 踩过：日志里一直冒 [sfud-x]）。*/
#if S31_SFUD_TRACE_XFER
    {
        rt_uint32_t i;
        rt_kprintf("[sfud-x] tx(%u):", (unsigned)write_size);
        for (i = 0; i < write_size && i < 16u; i++) {
            rt_kprintf(" %02X", write_buf[i]);
        }
        rt_kprintf("  rx(%u):", (unsigned)read_size);
        for (i = 0; i < read_size && i < 16u; i++) {
            rt_kprintf(" %02X", read_buf[i]);
        }
        rt_kprintf("  -> %d\n", (int)e);
    }
#endif
    return e;
}

/*---------------------------------------------------------------------------
 * SFUD 在 hardware_init 里回调：填钩子。
 *
 * 🚨 函数名**必须是** `sfud_spi_port_init`：sfud.c:252 把这个名字写死成
 *    `extern` 再调用（"port/sfup_port.c" 那个注释就是它），改不动 ——
 *    想改就得动 vendor 源码，那就不叫 vendor 了。
 *    ⇒ 因此**绝不能**把 RT-Thread 官方的
 *      `rt-thread/components/drivers/spi/dev_spi_flash_sfud.c` 加进编译：
 *      那份也定义了 `sfud_spi_port_init`（还有 `sf` 命令），会直接撞车。
 *      build.ps1 里只 glob 了 bsp\sfud\{src,port}，不会误收它。
 *-------------------------------------------------------------------------*/
sfud_err sfud_spi_port_init(sfud_flash *flash)
{
    RT_ASSERT(flash != RT_NULL);

    flash->spi.wr       = sfud_spi_wr;
    flash->spi.lock     = sfud_spi_lock;
    flash->spi.unlock   = sfud_spi_unlock;
    flash->spi.user_data = flash;
    flash->retry.delay  = sfud_retry_delay_100us;
    flash->retry.times  = 60u * 10000u;     /* 100us × 600000 ≈ 60 秒（一段写/擦的上限）*/

    return SFUD_SUCCESS;
}

/*---------------------------------------------------------------------------
 * 对外：probe
 *-------------------------------------------------------------------------*/
rt_err_t s31_sfud_bind(const char *spi_dev, rt_uint32_t max_hz)
{
    struct rt_spi_device *dev;
    struct rt_spi_configuration cfg;
    rt_err_t rc;

    if (spi_dev == RT_NULL) {
        spi_dev = "flash0";
    }
    if (max_hz == 0u) {
        max_hz = S31_SFUD_HZ_DEFAULT;
    }

    dev = (struct rt_spi_device *)rt_device_find(spi_dev);
    if (dev == RT_NULL ||
        dev->parent.type != RT_Device_Class_SPIDevice) {
        rt_kprintf("[sfud] 找不到 SPI 设备 \"%s\"（drv_spi.c 起来了吗？spi_pins 看看接线）\n",
                   spi_dev);
        return -RT_EIO;
    }

    /* 锁只建一次（重复 probe 换频率/重插都走这条路）*/
    if (!s_sfud_lock_ready) {
        rc = rt_mutex_init(&s_sfud_lock, "sfud", RT_IPC_FLAG_PRIO);
        if (rc != RT_EOK) {
            rt_kprintf("[sfud] 建互斥锁失败 rc=%d\n", (int)rc);
            return -RT_ENOMEM;
        }
        s_sfud_lock_ready = RT_TRUE;
    }

    /* flash 只认 mode0/8 位；每次 probe 都重配一遍（设备可能刚从 LCD 档切回来）
     *
     * 🚨 **`-RT_EBUSY` 不是错误，要放行**：RT-Thread 的 `rt_spi_bus_configure()`
     *    在"总线正被别的设备占着"时就返回它（源码注释原文：*RT_EBUSY is not an
     *    error condition and the configuration will take effect once the device
     *    has the bus*）—— 也就是配置**延迟生效**，等我们真的拿到总线时驱动会
     *    自己补配。踩过：跑完 `lcd`（总线归 lcd0）再 `sf probe` 直接报 rc=-7。*/
    cfg.data_width = 8;
    cfg.mode       = RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz     = max_hz;
    rc = rt_spi_configure(dev, &cfg);
    if (rc != RT_EOK && rc != -RT_EBUSY) {
        rt_kprintf("[sfud] rt_spi_configure 失败 rc=%d\n", (int)rc);
        return rc;
    }

    s_sfud_spi = dev;
    s_sfud_hz  = max_hz;
    return RT_EOK;
}

rt_err_t s31_sfud_probe(const char *flash_name, const char *spi_dev, rt_uint32_t max_hz)
{
    rt_err_t rc;
    sfud_flash *flash;

    if (spi_dev == RT_NULL) {
        spi_dev = "flash0";
    }

    rc = s31_sfud_bind(spi_dev, max_hz);
    if (rc != RT_EOK) {
        return rc;
    }

    /* ---- 设备表里那一份（不是副本！）---- */
    flash = sfud_get_device(0);
    if (flash == RT_NULL) {
        rt_kprintf("[sfud] SFUD 设备表是空的 —— sfud_cfg.h 的 SFUD_FLASH_DEVICE_TABLE 被改坏了？\n");
        s_sfud_spi = RT_NULL;
        s_sfud_hz  = 0u;
        return -RT_ERROR;
    }

    /* 名字：sfud_def.h 里是 `char *name`，所以得给它一块**活的**内存 */
    if (flash_name != RT_NULL) {
        rt_strncpy(s_sfud_name, flash_name, sizeof(s_sfud_name) - 1u);
        s_sfud_name[sizeof(s_sfud_name) - 1u] = '\0';
    } else {
        rt_strncpy(s_sfud_name, spi_dev, sizeof(s_sfud_name) - 1u);
        s_sfud_name[sizeof(s_sfud_name) - 1u] = '\0';
    }

    if (!flash->init_ok) {
        /* ⚠️ 只清"还没初始化过"的那次：已经初始化过的设备再清会把
         *    sfud_spi_port_init() 填好的钩子一起抹掉（sfud_device_init 之后
         *    不会再回调 port_init）。*/
        rt_memset(flash, 0, sizeof(sfud_flash));
    }
    flash->index    = 0;                 /* sfud_init() 也是这么设的 */
    flash->name     = s_sfud_name;
    flash->user_data = s_sfud_spi;       /* 顺手记下是哪个 RT-Thread SPI 设备 */
    flash->spi.name = (char *)spi_dev;   /* 只用于日志（rp2040 port 同款用法）*/

    /* 真正干活的一步：SFUD 会读 JEDEC ID → 试 SFDP → 查型号表 → 填参数
     * （识别完之后它自己会发 0x66+0x99 软复位，见 sfud.c:351，这里不用再补）*/
    if (sfud_device_init(flash) != SFUD_SUCCESS) {
        rt_kprintf("[sfud] 没识别出芯片 —— 先跑 `spi_id` 看 JEDEC ID（0x9F）：\n");
        rt_kprintf("        EF 40 17 = W25Q64（正常）；00 00 00 = MISO 悬空/模块没接；\n");
        rt_kprintf("        FF FF FF = 该脚被外部拉高（接了别的东西）；80/CC = 头几个沿没稳住\n");
        rt_kprintf("        也可以 `sf dbg` 直接看 0x9F / 0x90 / 0x5A 三条命令回来的原始字节\n");
        flash->init_ok  = RT_FALSE;
        flash->spi.name = RT_NULL;
        s_sfud_spi = RT_NULL;
        s_sfud_hz  = 0u;
        return -RT_ERROR;
    }

    rt_kprintf("[sfud] \"%s\" 挂上 SPI 设备 \"%s\"（%u Hz）\n",
               flash->name, spi_dev, (unsigned)s31_sfud_cur_hz());
    return RT_EOK;
}

rt_uint32_t s31_sfud_dev_num(void)
{
    sfud_flash *flash = sfud_get_device(0);

    return (flash != RT_NULL && flash->init_ok) ? 1u : 0u;
}

const char *s31_sfud_bind_dev(void)
{
    return (s_sfud_spi != RT_NULL) ? s_sfud_spi->parent.parent.name : RT_NULL;
}

rt_uint32_t s31_sfud_cur_hz(void)
{
    return s_sfud_hz;
}

void s31_sfud_unbind(void)
{
    sfud_flash *flash = sfud_get_device(0);

    if (s_sfud_lock_ready) {
        rt_mutex_take(&s_sfud_lock, RT_WAITING_FOREVER);
    }
    if (flash != RT_NULL) {
        flash->init_ok  = RT_FALSE;
        flash->spi.name = RT_NULL;
    }
    s_sfud_spi = RT_NULL;
    s_sfud_hz  = 0u;
    if (s_sfud_lock_ready) {
        rt_mutex_release(&s_sfud_lock);
    }
}

/*---------------------------------------------------------------------------
 * 裸事务（排错用）：不经过 SFUD，直接拿绑定的 RT-Thread 设备发一条命令。
 * 和 sfud_spi_read() 同一套切块规则（见文件头 ①）。
 *-------------------------------------------------------------------------*/
rt_err_t s31_sfud_raw_xfer(const rt_uint8_t *tx, rt_uint32_t tx_len,
                           rt_uint8_t *rx, rt_uint32_t rx_len)
{
    size_t off = 0;

    if (s_sfud_spi == RT_NULL) {
        return -RT_EIO;
    }
    if (tx_len > S31_SFUD_CMD_MAX || rx == RT_NULL || rx_len == 0u) {
        return -RT_EINVAL;
    }
    while (off < rx_len) {
        size_t n = rx_len - off;
        if (n > S31_SFUD_DATA_MAX) {
            n = S31_SFUD_DATA_MAX;
        }
        if (rt_spi_send_then_recv(s_sfud_spi, tx, tx_len, rx + off, n) != RT_EOK) {
            return -RT_EIO;
        }
        off += n;
    }
    return RT_EOK;
}
