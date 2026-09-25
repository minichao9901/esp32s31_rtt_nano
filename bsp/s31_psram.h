/*===========================================================================
 * s31_psram.h -- PSRAM 初始化与访问接口（ESP32-S31 / RT-Thread Nano 版）
 *
 * 实现体在 `bsp/s31_psram.c`（从 boot_msc_s31 移植 —— 那份是在真板上把
 * PSRAM 调通、并且踩完了全部坑的版本；坑的清单见那个文件的文件头 + README §5.10）。
 *
 * 为什么值得直接拿来用：PSRAM 这一块在本工作区是**最贵的一段**，
 * 真凶两条都不是"看手册能看出来的"：
 *   ① PSRAM/MPLL 挂在一个**专用 1.8V LDO** 上，上电默认关着（PMU +0x218/+0x1e8）；
 *   ② **PMA**（RISC-V 自定义 CSR `CSR_PMACFG15`）把外部存储窗口标成只读 ——
 *      它是 CPU 的 CSR，**寄存器 dump 里根本看不到**，现象是"一写就 access fault"。
 *
 * ⚠️ 本文件只声明"窗口地址 + 几个 API"，**不含任何 IDF 头**；
 *    IDF 的 LL 头只在 .c 里用（`bsp/idf_headers/`，冻结的 45 个头）。
 *===========================================================================*/
#ifndef S31_PSRAM_H
#define S31_PSRAM_H

#include <stdint.h>
#include "s31_regs.h"           /* S31_PSRAM_VADDR / S31_PSRAM_VSIZE */

/* PSRAM 映射窗口的起点与长度（= 0x50000000 / 64MB 窗口，实际芯片 16MB）*/
#define S31_PSRAM_BASE      S31_PSRAM_VADDR

/*---------------------------------------------------------------------------
 * 初始化（幂等；0 = 成功，负 = 具体失败点）
 *   顺序：模拟子系统前置 -> 1.8V LDO -> MPLL 400MHz -> PSRAM 控制器时序
 *         -> 写 mode register -> 连接性自检 -> PMA 补写权限 -> MMU 建映射
 * 完成后 `(volatile uint32_t *)S31_PSRAM_BASE` 就是可读写的普通内存。
 *-------------------------------------------------------------------------*/
int  s31_psram_init(void);

/* 初始化是否成功过（后面加的命令行都用它把关）*/
int  s31_psram_ready(void);

/* 芯片容量（字节，从 PSRAM 的 size 寄存器读；失败返回 0）*/
uint32_t s31_psram_size(void);

/*---------------------------------------------------------------------------
 * 绕过 cache 的读写（走 MSPI 原始事务）
 *   - 用途：写进去之后**不依赖 cache** 就能回读校验（排查 cache 假象）
 *   - paddr 是**相对 PSRAM 起点的物理偏移**（不是 0x50000000 那个虚拟地址）
 *-------------------------------------------------------------------------*/
void s31_psram_write_raw(uint32_t paddr, const void *buf, uint32_t len);
void s31_psram_read_raw (uint32_t paddr, void *buf, uint32_t len);

/* 让一段虚拟地址的 cache 行失效（改完 PSRAM 内容后、读之前调）*/
void s31_psram_invalidate_cache(uint32_t vaddr, uint32_t len);

/* 自检：把 `bytes` 从 `base` 开始做 pattern 写 + 读回校验 + CRC 比对。
 * 返回 0 = 通过；非 0 = 出错的字节数（0xFFFFFFFF 表示参数/状态不对）。*/
uint32_t s31_psram_crc_test(uint32_t base, uint32_t bytes);

#endif /* S31_PSRAM_H */
