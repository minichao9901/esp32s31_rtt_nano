/*===========================================================================
 * sfud_port.h -- SFUD 挂到本 port 的 SPI 总线上的那层胶水
 *
 * 只有三件事：
 *   ① `s31_sfud_probe()`  把 SFUD 引擎接到一个 RT-Thread SPI 设备上并识别芯片
 *   ② `s31_sfud_bind_dev()` / `s31_sfud_dev_num()` / `s31_sfud_dev()`  取回设备
 *   ③ `s31_sfud_unbind()` 让 SFUD 撒手（不再碰总线）
 *
 * 公共头 **不 include sfud.h**：调用者（app/main.c 等）只用本文件的 API，
 * 需要 sfud_flash 细节时自己 `#include <sfud.h>`（include 路径见 build.ps1）。
 *===========================================================================*/
#ifndef S31_SFUD_PORT_H
#define S31_SFUD_PORT_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * 一次 SPI 事务里，命令/地址阶段与数据阶段各自最长多少字节。
 *
 * 🚨 这是本 port 最要紧的一个数：`drv_spi.c` 走的是 **CPU 轮询（PIO）** 通路，
 *    收发各只有一个 16 字（64 字节）的 FIFO，`S31_SPI_PIO_MAX` 卡在 64。
 *    超了驱动会 rt_set_errno(-RT_EINVAL) 并**返回 0 字节**（不报错、不崩，
 *    就是传输"没发生"）—— 所以这一层必须自己切块，一次都不能超。
 *
 *    切块是安全的：中间各块靠 `SPI_MISC.cs_keep_active` 把 CS 一直压低
 *    （drv_spi.c 的 cs_take/cs_release 就是干这个的），对 flash 来说
 *    "命令+地址+一大段数据"仍然是**一次连续片选**。
 *-------------------------------------------------------------------------*/
#define S31_SFUD_CMD_MAX    8u
#define S31_SFUD_DATA_MAX   64u

/*---------------------------------------------------------------------------
 * 把 SFUD 接到一个 SPI 设备上并识别芯片。
 *
 *   flash_name : 给这块 flash 起的名字（SFUD 只在日志里用，可传 NULL）
 *   spi_dev    : RT-Thread 的 SPI 设备名。默认那套接线是 "flash0"
 *                （drv_spi.c：SCK=GPIO43 MOSI=44 MISO=45 CS=46，
 *                 都在 J2 上：17/18/15/16 脚）
 *   max_hz     : SPI 时钟（0 = 用 S31_SFUD_HZ_DEFAULT）
 *
 * 返回 0 = 成功；负 = 失败（-RT_EIO 找不到设备 / -RT_ERROR 识别不了芯片 /
 * -RT_ENOMEM 锁建不起来）。**失败时不会把总线留在半配置状态**。
 *
 * 可反复调用（换设备名/换频率/热插拔后重试都行）：第二次进来会把上一次的
 * 绑定覆盖掉，锁只建一次。
 *-------------------------------------------------------------------------*/
rt_err_t s31_sfud_probe(const char *flash_name, const char *spi_dev, rt_uint32_t max_hz);

/*---------------------------------------------------------------------------
 * 只绑定（配好 SPI 设备 + 建锁），**不跑 SFUD 的识别**。
 *
 * 存在的唯一理由是排错：`sf dbg` 要在 "SFUD 初始化之前" 打一次裸读，
 * 才能把「总线本身不通」和「SFUD 跑完之后才不通」分开 —— 否则
 * `sf probe` 失败时你没法判断该查接线还是查驱动。
 * probe() 内部也走这个函数。
 *-------------------------------------------------------------------------*/
rt_err_t s31_sfud_bind(const char *spi_dev, rt_uint32_t max_hz);

/* 默认 SPI 时钟：20MHz。W25Q64 的 0x03 单线读上限是 50MHz，
 * 但本 port 的 PIO 通路每 64 字节要重配一次 FIFO，先给个稳妥值；
 * 要试更快用 `sf clk <hz>`。*/
#define S31_SFUD_HZ_DEFAULT     20000000u

/* 当前绑定着几个设备（0 = 没绑定） */
rt_uint32_t s31_sfud_dev_num(void);
/* 当前绑定的 SPI 设备名（没绑定时返回 NULL） */
const char *s31_sfud_bind_dev(void);
/* 撒手：之后 SFUD 的 API 都用不了（再 probe 一次即可恢复） */
void s31_sfud_unbind(void);

/* 当前 SPI 时钟（Hz；没绑定时 0）—— `sf clk` 显示用 */
rt_uint32_t s31_sfud_cur_hz(void);

/*---------------------------------------------------------------------------
 * 裸事务（排错用：**不经过 SFUD**，直接拿 RT-Thread 设备发一条命令）
 *
 *   tx 最多 S31_SFUD_CMD_MAX 字节（命令+地址），rx 想收多少都行（内部按
 *   S31_SFUD_DATA_MAX 切块，中间 CS 一直低）。
 *
 * 用途：`sf dbg` 要在"SFUD 根本没识别成功"的情况下看 0x9F / 0x90 / 0x5A
 * 回来的原始字节 —— 那时候 flash 结构体里还没有 wr 钩子可用。
 * 没绑定设备时返回 -RT_EIO。
 *-------------------------------------------------------------------------*/
rt_err_t s31_sfud_raw_xfer(const rt_uint8_t *tx, rt_uint32_t tx_len,
                           rt_uint8_t *rx, rt_uint32_t rx_len);

/*---------------------------------------------------------------------------
 * 数据相位的切块大小（默认 64 = PIO 上限；只能往小调）
 *
 * 给 `sf bench` 用的：把块长调小，就能量出"每次 SPI 事务的固定开销"——
 * 这是**真的在改 SPI 事务尺寸**，不是只改上层调用长度（上层 `sfud_read`
 * 想读多少都行，到了移植层照样按这个值切）。
 *   n = 0 当 1；n > 64 截成 64（PIO 一次就这么多）。
 *-------------------------------------------------------------------------*/
void s31_sfud_set_chunk(rt_uint32_t n);
rt_uint32_t s31_sfud_get_chunk(void);

#ifdef __cplusplus
}
#endif

#endif /* S31_SFUD_PORT_H */
