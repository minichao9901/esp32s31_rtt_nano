/*===========================================================================
 * drv_flash_onboard.c -- 板载 16MB SPI flash：划 8MB 出来做 RT-Thread 块设备
 *
 * 为什么需要它：外接 flash 有官方 SFUD 组件就够了（bsp/drv_spi_flash.c），
 * 但**板载 flash 是我们自己正跑着的那一片**（app.bin 烧在 0x2000），RT-Thread
 * 下没有任何现成驱动能碰它。要挂 FAT / 给 CherryUSB MSC 当盘，就得自己提供
 * 一个块设备 —— 这个文件就是那一个（`onboard0`，16384 × 512B = 8MB）。
 *
 * 底层走 **ROM 的 legacy spi_flash API**（esp_rom_spiflash_read/write/
 * erase_sector/unlock/config_param），地址在 bsp/linker.ld 里 PROVIDE
 * （出处 components/esp_rom/esp32s31/ld/esp32s31.rom.ld）。为什么用 ROM
 * 而不是自己戳 SPI1(MSPI) 寄存器：
 *   · 它就是 IDF 二级 bootloader 用的同一套，**擦写期间 SPI1 与 cache 的互斥
 *     由 ROM 自己处理**（我们没链 IDF，拿不到 cache_disable/IPI 那套设施）；
 *   · 本工程代码/数据**全在内部 RAM**（linker.ld：0x2F000000 起；SRAM 不走
 *     cache），所以操作期间不会有 flash 取指/取数 —— 只要**关中断**就够。
 *     （boot_msc_s31 用同一套 ROM 接口真板实测过：擦后读回全 FF、写后读回逐字节一致。）
 *
 * 区域划分（16MB 全片，详见工程 README「flash 布局」）：
 *   0x000000 ┌────────────────────────────────┐
 *            │ ROM 保留 / 分区表区（不用）     │
 *   0x002000 │ 本固件 app.bin（~90KB）         │ ← bootROM 从这里加载二级镜像
 *   0x100000 │ 自检扇区（4KB，本驱动独占）     │
 *   0x400000 │ ★ 磁盘区 8MB → `onboard0`       │ ← 挂 FAT / 给 PC 当 MSC 盘
 *   0xC00000 │ 空闲                            │
 *   0x1000000└────────────────────────────────┘
 *   🚨 磁盘区**绝不能**和 app.bin 重叠：擦掉正在跑的镜像 = 立刻变砖。
 *
 * 🚨 两个真板踩过的坑（都来自 boot_msc_s31，别重犯）：
 *   ① **bootROM 交棒时 ROM 记的 chip_size 只有 2MB**（实测 device_id=0x00464018
 *      但 chip_size=2097152）。而 ROM 的擦/写/读**都会先做范围检查**，超了就直接
 *      返回 ERR 且根本不碰 flash —— 现象是"擦失败、写完读回全 0"，极容易被误判成
 *      "寄存器被搞坏了"。解法就是 IDF 自己用的那个官方接口
 *      `esp_rom_spiflash_config_param()`，把真实容量告诉 ROM（见 s31_ob_hw_init）。
 *   ② 写之前必须**先擦**（NOR 只能把 1 写成 0）—— 本驱动在 write 里做了
 *      读-改-擦-写，并顺手跳过两种"其实不用擦"的情况（见 s31_ob_write）。
 *===========================================================================*/

#include <rtthread.h>
#include <rtdevice.h>

/* ROM 的 legacy spi_flash API。签名与 `esp_rom_spiflash.h` 一致，但那份头会
 * `#include "esp32s31/rom/spi_flash.h"`，而 bsp/idf_headers 是扁平摆放的
 * （没有 esp32s31/ 这一层），所以这里自己声明 5 个原型（地址见 linker.ld）。*/
extern int esp_rom_spiflash_read(uint32_t src_addr, uint32_t *dest, int32_t len);
extern int esp_rom_spiflash_write(uint32_t dest_addr, const uint32_t *src, int32_t len);
extern int esp_rom_spiflash_erase_sector(uint32_t sector_num);
extern int esp_rom_spiflash_unlock(void);
extern int esp_rom_spiflash_config_param(uint32_t deviceId, uint32_t chip_size, uint32_t block_size,
                                         uint32_t sector_size, uint32_t page_size, uint32_t status_mask);

/* ROM 记的 flash 芯片结构体指针（boot_msc_s31 反汇编 0x2f80ef70 得到的结论：
 * 原始 erase 先做 `sector_num < chip_size/sector_size` 检查，用的就是这个指针里的字段）*/
#define S31_OB_ROM_CHIP_PTR   0x2f07ffe0u

#define S31_OB_FLASH_SIZE     0x1000000u    /* 16MB 全片 */
#define S31_OB_ERASE_SIZE     0x1000u       /* 4KB 擦除粒度 */
#define S31_OB_SCRATCH_ADDR   0x100000u     /* 自检扇区（磁盘区之外，独占 4KB）*/
#define S31_OB_SCRATCH_MAGIC  0x4F42464Cu   /* 'OBFL' */
#define S31_OB_DISK_ADDR      0x400000u     /* 磁盘区起点 = 4MB */
#define S31_OB_DISK_SIZE      0x800000u     /* 磁盘区大小 = 8MB */
#define S31_OB_BLK_SIZE       512u          /* 块设备扇区 = 512B（FAT/MSC 都用它）*/
#define S31_OB_BLK_COUNT      (S31_OB_DISK_SIZE / S31_OB_BLK_SIZE)   /* 16384 */

#define S31_OB_DEV_NAME       "onboard0"

static struct rt_device s_ob_dev;
static struct rt_mutex  s_ob_lock;                                          /* 串行化"读-改-擦-写" */
static rt_uint8_t       s_ob_rmw[S31_OB_ERASE_SIZE] __attribute__((aligned(8)));  /* 4KB 块镜像 */

/*===========================================================================
 * ROM 包装：读写擦期间关中断
 *
 * 为什么关中断就够：本工程代码/数据都在内部 RAM，正常执行不会碰 flash；
 * 关中断是为了挡住"ISR 里访问 flash 里的常量"这种意外（也正是 boot_msc_s31
 * 真板验证过的做法）。代价：一次 4KB 扇区擦 ~45ms 内 tick 不走 —— RT-Thread
 * 只是少几个 tick，不影响正确性，但控制台/USB 的中断会晚一点响应。
 *===========================================================================*/
static int s31_ob_rom_read(rt_uint32_t addr, void *dst, rt_uint32_t len)
{
    rt_base_t level;
    int r;

    if (len == 0u)
    {
        return 0;
    }

    level = rt_hw_interrupt_disable();
    if ((((rt_uint32_t)(rt_ubase_t)dst | len) & 3u) == 0u)
    {
        r = esp_rom_spiflash_read(addr, (uint32_t *)dst, (int32_t)len);
        rt_hw_interrupt_enable(level);
    }
    else
    {
        /* 非对齐：按 4 字节粒度整段走中转。中转缓冲放**栈上**（不是 static），
         * 否则两个线程同时走这条路会互相踩（关中断只挡中断，挡不住线程切换）。
         * ⚠️ 关中断**只在每次 ROM 调用前后**，块与块之间放开来 ——
         * 关一整段（几 ms）会把别人的采样时钟/网络/串口全拖住（踩过：音频 ISR 风暴）。*/
        rt_uint8_t tmp[72] __attribute__((aligned(8)));
        rt_uint8_t *out = (rt_uint8_t *)dst;
        rt_uint32_t done = 0u;

        rt_hw_interrupt_enable(level);
        r = 0;
        while ((done < len) && (r == 0))
        {
            rt_uint32_t n = len - done;
            rt_uint32_t base = (addr + done) & ~3u;
            rt_uint32_t head = (addr + done) - base;
            rt_uint32_t words;

            if (n > (rt_uint32_t)sizeof(tmp) - 4u - head)
            {
                n = (rt_uint32_t)sizeof(tmp) - 4u - head;
            }
            words = (head + n + 3u) & ~3u;
            level = rt_hw_interrupt_disable();
            r = esp_rom_spiflash_read(base, (uint32_t *)tmp, (int32_t)words);
            rt_hw_interrupt_enable(level);
            if (r == 0)
            {
                rt_memcpy(out + done, tmp + head, n);
            }
            done += n;
        }
    }
    return r;
}

/* 擦：addr 必须 4KB 对齐 */
static int s31_ob_rom_erase(rt_uint32_t addr)
{
    rt_base_t level;
    int r;

    level = rt_hw_interrupt_disable();
    r = esp_rom_spiflash_erase_sector(addr / S31_OB_ERASE_SIZE);
    rt_hw_interrupt_enable(level);
    return r;
}

/* 写：addr 与 len 都要求 4 字节对齐；调用者负责先擦除 */
static int s31_ob_rom_write(rt_uint32_t addr, const void *src, rt_uint32_t len)
{
    rt_base_t level;
    int r;

    if (len == 0u)
    {
        return 0;
    }

    level = rt_hw_interrupt_disable();
    r = esp_rom_spiflash_write(addr, (const uint32_t *)src, (int32_t)len);
    rt_hw_interrupt_enable(level);
    return r;
}

/*===========================================================================
 * 硬件初始化：把真实容量告诉 ROM + 清写保护
 *===========================================================================*/
static rt_err_t s31_ob_hw_init(void)
{
    rt_uint32_t chip = *(volatile rt_uint32_t *)S31_OB_ROM_CHIP_PTR;
    rt_uint32_t device_id = chip ? *(volatile rt_uint32_t *)chip : 0u;

    /* 🚨 不调这一句，2MB 以上全是"擦不动/写不进"（见文件头坑①）*/
    if (esp_rom_spiflash_config_param(device_id, S31_OB_FLASH_SIZE, 64u * 1024u,
                                      S31_OB_ERASE_SIZE, 256u, 0xFFFFu) != 0)
    {
        rt_kprintf("[onboard] config_param 失败（device_id=0x%08x）\n", (unsigned)device_id);
        return -RT_ERROR;
    }

    /* 清块保护位（有些模块出厂带 BP；不清的话高地址擦写会被静默拒绝）*/
    if (esp_rom_spiflash_unlock() != 0)
    {
        rt_kprintf("[onboard] unlock 失败\n");
        return -RT_ERROR;
    }

    return RT_EOK;
}

/*===========================================================================
 * 自检：在自检扇区里擦→写→读回
 *
 * 为什么要自检：ROM 那条路"参数不对就静默失败"，不验一次根本不知道能不能写。
 * 标记对得上就只读一次（不擦，省 45ms 和一次 P/E 寿命）；对不上才做完整的擦写读。
 *===========================================================================*/
static rt_err_t s31_ob_selftest(rt_bool_t force)
{
    rt_uint32_t magic;

    if (!force)
    {
        magic = 0u;
        if ((s31_ob_rom_read(S31_OB_SCRATCH_ADDR, &magic, 4u) == 0) &&
            (magic == S31_OB_SCRATCH_MAGIC))
        {
            rt_kprintf("[onboard] 写通路自检：OK（0x%06x 有上次写下的标记，读回一致）\n",
                       (unsigned)S31_OB_SCRATCH_ADDR);
            return RT_EOK;
        }
    }

    if (s31_ob_rom_erase(S31_OB_SCRATCH_ADDR) != 0)
    {
        rt_kprintf("[onboard] 自检失败：擦 0x%06x 失败\n", (unsigned)S31_OB_SCRATCH_ADDR);
        return -RT_ERROR;
    }

    magic = S31_OB_SCRATCH_MAGIC;
    if (s31_ob_rom_write(S31_OB_SCRATCH_ADDR, &magic, 4u) != 0)
    {
        rt_kprintf("[onboard] 自检失败：写 0x%06x 失败\n", (unsigned)S31_OB_SCRATCH_ADDR);
        return -RT_ERROR;
    }

    magic = 0u;
    if ((s31_ob_rom_read(S31_OB_SCRATCH_ADDR, &magic, 4u) != 0) ||
        (magic != S31_OB_SCRATCH_MAGIC))
    {
        rt_kprintf("[onboard] 自检失败：写后读回是 0x%08x（期望 0x%08x）\n",
                   (unsigned)magic, (unsigned)S31_OB_SCRATCH_MAGIC);
        return -RT_ERROR;
    }

    rt_kprintf("[onboard] 写通路自检：OK（擦→写→读回 0x%06x 一致）\n",
               (unsigned)S31_OB_SCRATCH_ADDR);
    return RT_EOK;
}

/*===========================================================================
 * 块设备接口
 *
 * 扇区 = 512B（不是擦除粒度）：FAT 与 USB MSC 都是这个尺寸，对上以后
 * `dfs_mount` 和 MSC 的 LBA 可以共用同一个块设备，谁也不用换算。
 * 擦除粒度那 4KB 的账由 write 内部的"读-改-擦-写"吃掉。
 *===========================================================================*/
static rt_err_t s31_ob_init(rt_device_t dev)  { (void)dev; return RT_EOK; }
static rt_err_t s31_ob_open(rt_device_t dev, rt_uint16_t oflag) { (void)dev; (void)oflag; return RT_EOK; }
static rt_err_t s31_ob_close(rt_device_t dev) { (void)dev; return RT_EOK; }

static rt_ssize_t s31_ob_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_uint32_t off;
    rt_uint32_t len;

    (void)dev;
    if ((pos < 0) || (size == 0u) || (buffer == RT_NULL))
    {
        return 0;
    }
    if ((rt_uint32_t)pos + (rt_uint32_t)size > S31_OB_BLK_COUNT)
    {
        return 0;       /* 越界：返回 0 = 失败（dfs/MSC 都按这个判错）*/
    }

    off = S31_OB_DISK_ADDR + (rt_uint32_t)pos * S31_OB_BLK_SIZE;
    len = (rt_uint32_t)size * S31_OB_BLK_SIZE;

    if (s31_ob_rom_read(off, buffer, len) != 0)
    {
        return 0;
    }
    return (rt_ssize_t)size;
}

/* 写：按 4KB 擦除块做"读-改-擦-写"，三种情况都在这一遍循环里处理：
 *   ① 目标块内容 == 要写的内容 → 什么都不做（FAT 会反复重写同一份目录/FAT 表）；
 *   ② 新内容每个 bit 都能靠"把 1 写成 0"得到（即 (~old & new) == 0）→ **不用擦**，
 *      直接再编程一次（NOR 允许对同一块多次编程，只要只把 1 变 0）—— 省 ~45ms；
 *   ③ 其余 → 擦掉整块，再把补齐后的整块写回去。
 * 判断都建立在"先把整块读出来"之上：一次 4KB 读 ~0.5ms，换掉动辄 45ms 的擦除。
 * 逐**字节**比较而不是按字：调用者的缓冲可能只有 1 字节对齐（rv32 上 lw 不对齐会 trap）。*/
static rt_ssize_t s31_ob_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    const rt_uint8_t *src = (const rt_uint8_t *)buffer;
    rt_uint32_t off;
    rt_uint32_t left;

    (void)dev;
    if ((pos < 0) || (size == 0u) || (buffer == RT_NULL))
    {
        return 0;
    }
    if ((rt_uint32_t)pos + (rt_uint32_t)size > S31_OB_BLK_COUNT)
    {
        return 0;
    }

    off  = S31_OB_DISK_ADDR + (rt_uint32_t)pos * S31_OB_BLK_SIZE;
    left = (rt_uint32_t)size * S31_OB_BLK_SIZE;

    if (rt_mutex_take(&s_ob_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return 0;
    }

    while (left > 0u)
    {
        rt_uint32_t blk = off & ~(S31_OB_ERASE_SIZE - 1u);   /* 本 4KB 块首 */
        rt_uint32_t in  = off - blk;                         /* 块内偏移 */
        rt_uint32_t n   = S31_OB_ERASE_SIZE - in;            /* 到块尾的字节数 */
        rt_uint32_t i;
        rt_bool_t need_erase = RT_FALSE;
        rt_bool_t same = RT_TRUE;
        int rc = 0;

        if (n > left)
        {
            n = left;
        }

        /* 整块读到中转缓冲（整块写时 in=0、n=4KB，就是全部）*/
        if (s31_ob_rom_read(blk, s_ob_rmw, S31_OB_ERASE_SIZE) != 0)
        {
            break;
        }

        for (i = 0u; i < S31_OB_ERASE_SIZE; i++)
        {
            rt_uint8_t old = s_ob_rmw[i];
            rt_uint8_t nv  = ((i >= in) && (i < in + n)) ? src[i - in] : old;

            if (nv != old)
            {
                same = RT_FALSE;
            }
            if ((rt_uint8_t)(~old & nv) != 0u)
            {
                need_erase = RT_TRUE;
            }
            s_ob_rmw[i] = nv;
        }

        if (!same)
        {
            if (need_erase)
            {
                rc = s31_ob_rom_erase(blk);
            }
            if (rc == 0)
            {
                rc = s31_ob_rom_write(blk, s_ob_rmw, S31_OB_ERASE_SIZE);
            }
            if (rc != 0)
            {
                break;
            }
        }

        off  += n;
        src  += n;
        left -= n;
    }

    rt_mutex_release(&s_ob_lock);

    if (left != 0u)
    {
        return 0;       /* 中途失败 */
    }
    return (rt_ssize_t)size;
}

static rt_err_t s31_ob_control(rt_device_t dev, int cmd, void *args)
{
    (void)dev;

    switch (cmd)
    {
    case RT_DEVICE_CTRL_BLK_GETGEOME:
    {
        struct rt_device_blk_geometry *g = (struct rt_device_blk_geometry *)args;
        if (g == RT_NULL)
        {
            return -RT_ERROR;
        }
        g->sector_count     = S31_OB_BLK_COUNT;
        g->bytes_per_sector = S31_OB_BLK_SIZE;
        g->block_size       = S31_OB_ERASE_SIZE;    /* 建议一次连续写的粒度 */
        return RT_EOK;
    }
    case RT_DEVICE_CTRL_BLK_ERASE:
    {
        /* 语义同官方 SFUD 移植层：args = {start_sector, end_sector}，前闭后开；
         * start == end 时按"擦一个扇区"处理。擦除要对齐 4KB 块，所以两头各自取整。*/
        rt_uint32_t *addrs = (rt_uint32_t *)args;
        rt_uint32_t start, end, blk;

        if ((addrs == RT_NULL) || (addrs[0] >= S31_OB_BLK_COUNT) || (addrs[1] > S31_OB_BLK_COUNT))
        {
            return -RT_ERROR;
        }
        start = addrs[0];
        end   = (addrs[1] == addrs[0]) ? (addrs[0] + 1u) : addrs[1];
        if (end > S31_OB_BLK_COUNT)
        {
            return -RT_ERROR;
        }

        blk = (S31_OB_DISK_ADDR + start * S31_OB_BLK_SIZE) & ~(S31_OB_ERASE_SIZE - 1u);
        for (; blk < S31_OB_DISK_ADDR + end * S31_OB_BLK_SIZE; blk += S31_OB_ERASE_SIZE)
        {
            if (s31_ob_rom_erase(blk) != 0)
            {
                return -RT_ERROR;
            }
        }
        return RT_EOK;
    }
    case RT_DEVICE_CTRL_BLK_SYNC:
        return RT_EOK;      /* 没有写缓存，无需同步 */
    default:
        return -RT_EINVAL;
    }
}

/*===========================================================================
 * msh 命令：onboard
 *===========================================================================*/
/* 直读块设备的带宽（`onboard -b [KB]`，默认 256KB）：
 * 为什么要有它：文件系统读慢时，得先分清是"块设备本身慢"还是"elmfat 慢"。
 * 4KB 一块、绕开 DFS，只走 s31_ob_read → ROM 读。*/
static void s31_ob_bench(rt_uint32_t kb)
{
    static rt_uint8_t buf[4096];
    rt_uint32_t blks = (kb * 1024u) / 4096u;
    rt_uint32_t i, blk = 0;
    rt_tick_t t0;
    rt_uint32_t ms, bps;

    if (blks == 0u)
    {
        blks = 1u;
    }
    t0 = rt_tick_get();
    for (i = 0; i < blks; i++)
    {
        if (rt_device_read(&s_ob_dev, (rt_off_t)(blk * 8u), buf, 8u) != 8u)
        {
            rt_kprintf("[onboard] 读失败 @块 %u\n", (unsigned)blk);
            return;
        }
        blk++;
        if (blk * 8u >= S31_OB_BLK_COUNT)
        {
            blk = 0u;
        }
    }
    ms  = (rt_uint32_t)(rt_tick_get() - t0);
    bps = ms ? (rt_uint32_t)((rt_uint64_t)blks * 4096u * 1000u / ms) : 0u;
    rt_kprintf("[onboard] 直读 %u KB 用了 %u ms → %u kB/s（%u us/4KB 块）\n",
               (unsigned)(blks * 4u), (unsigned)ms, (unsigned)(bps / 1024u),
               (unsigned)(ms * 1000u / blks));
}

static void onboard(int argc, char **argv)
{
    if ((argc >= 2) && ((rt_strcmp(argv[1], "-t") == 0) || (rt_strcmp(argv[1], "test") == 0)))
    {
        rt_kprintf("[onboard] 强制自检（擦/写 0x%06x 那一个扇区）...\n",
                   (unsigned)S31_OB_SCRATCH_ADDR);
        s31_ob_selftest(RT_TRUE);
        return;
    }
    if ((argc >= 2) && (rt_strcmp(argv[1], "-b") == 0))
    {
        s31_ob_bench((argc >= 3) ? (rt_uint32_t)atoi(argv[2]) : 256u);
        return;
    }

    rt_kprintf("板载 flash 磁盘 %s : %u 扇区 x %u B = %u KB\n",
               S31_OB_DEV_NAME, (unsigned)S31_OB_BLK_COUNT, (unsigned)S31_OB_BLK_SIZE,
               (unsigned)(S31_OB_DISK_SIZE / 1024u));
    rt_kprintf("  flash 偏移 : 0x%06x .. 0x%06x（4KB 擦除粒度，驱动内部读-改-擦-写）\n",
               (unsigned)S31_OB_DISK_ADDR, (unsigned)(S31_OB_DISK_ADDR + S31_OB_DISK_SIZE));
    rt_kprintf("  自检扇区   : 0x%06x（`onboard -t` 强制重跑一次擦/写/读自检）\n",
               (unsigned)S31_OB_SCRATCH_ADDR);
    rt_kprintf("  用法       : mkfs onboard0 -> mount onboard0 / elm -> ls / -> df\n");
    rt_kprintf("               onboard -b [KB]   直读块设备测带宽（默认 256KB）\n");
    rt_kprintf("               ⚠️ 第一个文件系统要挂 \"/\"（DFS v1 没有虚拟根目录，见 README 坑 28）\n");
}
MSH_CMD_EXPORT(onboard, show/verify onboard flash disk (onboard [-t]));

/*===========================================================================
 * 初始化（组件级 .rti_fn.4：设备框架、堆、控制台都已就绪）
 *===========================================================================*/
static int s31_onboard_flash_init(void)
{
    rt_err_t rc;

    if (s31_ob_hw_init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    /* 静态互斥量要用 rt_mutex_init：rt_mutex_create 会另外从堆里分配对象，
     * 返回的指针不是我们那个结构体（曾经想当然踩过）。*/
    if (rt_mutex_init(&s_ob_lock, "obfl", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_kprintf("[onboard] 互斥量初始化失败\n");
        return -RT_ERROR;
    }

    s_ob_dev.type    = RT_Device_Class_Block;
    s_ob_dev.init    = s31_ob_init;
    s_ob_dev.open    = s31_ob_open;
    s_ob_dev.close   = s31_ob_close;
    s_ob_dev.read    = s31_ob_read;
    s_ob_dev.write   = s31_ob_write;
    s_ob_dev.control = s31_ob_control;

    rc = rt_device_register(&s_ob_dev, S31_OB_DEV_NAME,
                            RT_DEVICE_FLAG_RDWR);
    /* 🚨 **别加 RT_DEVICE_FLAG_STANDALONE**（2026-09-26 为这个白查了两轮）：
     * STANDALONE 的语义是"这个设备只能被打开一次"——`rt_device_open()` 在
     * **调用驱动的 open 回调之前**就会对着 `open_flag & RT_DEVICE_OFLAG_OPEN`
     * 判一次，第二次直接返回 -RT_EBUSY。
     * 后果：`mount onboard0 / elm` 成功后设备一直开着（ref=1），此后
     *   · 再 `mount`/`umount` 后 `mount` → 失败；
     *   · `msc start` 之后（官方 demo 打开块设备且**从不 close**）→ 失败；
     * 而 `dfs_mount()` 收到 -EBUSY 会**在调文件系统 mount 之前**就返回 -1，
     * 报错只有一句 "mount ... failed!"，连 fatfs 的大门都没进 —— 极难往
     * "设备被打开过"这个方向想（我们最后是靠临时打印驱动的 open 回调才定位到）。
     * 官方 SFUD 那个块设备是带 STANDALONE 的（`dev_spi_flash_sfud.c`），
     * 所以外接 flash 上也有同样的脾气：**同一个启动周期里只能挂一次**。
     * 块设备和文件系统天然要被"挂载 + MSC"两头打开，所以这里必须去掉。*/
    if (rc != RT_EOK)
    {
        rt_kprintf("[onboard] 注册块设备 %s 失败: %d\n", S31_OB_DEV_NAME, rc);
        return rc;
    }

    rt_kprintf("[onboard] 板载 flash 磁盘就绪：%s = %u 扇区 x %uB = %u KB（@0x%06x）\n",
               S31_OB_DEV_NAME, (unsigned)S31_OB_BLK_COUNT, (unsigned)S31_OB_BLK_SIZE,
               (unsigned)(S31_OB_DISK_SIZE / 1024u), (unsigned)S31_OB_DISK_ADDR);

    /* 自检失败不阻断启动：盘先用不了，其它功能照常 */
    s31_ob_selftest(RT_FALSE);
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(s31_onboard_flash_init);
