# bsp/idf_headers —— 冻结进来的 ESP-IDF 头文件

> **这个目录是自动生成的，别手改。**
> 重新生成：`pwsh -File tools\sync_idf_headers.ps1`（或 `make headers`）

## 这是什么

`bsp/s31_psram.c` 要用 IDF 的 LL 头（`hal/psram_ctrlr_ll.h`、`hal/mspi_ll.h`、
`hal/mmu_ll.h`、`soc/cache_reg.h`）——**按字段名写寄存器，不手抄位号**。
本工作区因为手抄位号栽过两次：

- `LP_AONCLKRST_MSPI_DIV.FB_DIV` 实际在 **bit[7:3]**，写在了 bit[4:0]；
- AP octal PSRAM 的 **MR0 / MR4 / MR8** 三个位域全凭印象拍错。

两次都是"寄存器写错、不报错、只是行为不对"，各花掉一整轮排查。用 IDF 的头之后，
这类错误在**编译期**就没了。

但那 4 个头会拖出一整个传递闭包（大头是 `soc/spi_mem_*_struct.h` 那种把每个寄存器
都列一遍的文件），所以把它们**冻结进工程**，从此编译不再需要 IDF 源码树。

> 📌 这份目录是从 `boot_msc_s31/bsp/idf_headers/` 抄来的（那边先落的这套办法）。
> ⚠️ 但脚本里 **RT-Thread 的 include 路径是后补的** —— 本工程是 RT-Thread，
> `gcc -M` 问依赖时必须带上 `rt-thread/include` 等，否则连 `rtthread.h` 都找不到。

**⇒ 本工程现在是 100% 自包含的**：换台机器只要有 RISC-V 工具链就能编。

## 怎么生成的

`tools/sync_idf_headers.ps1` **不自己解析 `#include`**（那样会漏掉条件编译、
以及 `#include "sibling.h"` 这种靠"同级目录"解析的形式），
而是用 `gcc -M` **问编译器**：把依赖表里所有落在 IDF 树里的头文件全部拷过来。

- 目录形状 = 每个头**相对命中它的那个 include 根**的路径
  （因为 include 字符串本身就是将来在这个扁平目录下的相对路径）；
- 顺序 = 编译器搜索 `-I` 的顺序，所以"第一个命中"和编译器看到的完全一致；
- **45 个文件 / 约 2.4 MB**，来源见 [`_SOURCE.txt`](_SOURCE.txt)（含 IDF 版本与 commit）。

## 什么时候需要重新同步

只有当 IDF 那边 **S31 的寄存器定义被修正** 时才需要，比如：

- S31 从 preview target 转正、寄存器头跟着改；
- 发现某个 `*_reg.h` 的位域定义有误。

那时跑一次 `sync_idf_headers.ps1`，然后**重新编译 + 上板验证**即可
（`_SOURCE.txt` 会记下新的版本与时间戳）。

## 许可

这些文件来自 [ESP-IDF](https://github.com/espressif/esp-idf)，
版权归 Espressif Systems 所有，**Apache License 2.0**。
原样复制、未做修改；每个文件里都保留着它自己的版权头。
