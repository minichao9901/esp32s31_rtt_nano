/*===========================================================================
 * linker.ld -- RT-Thread Nano on ESP32-S31（bootROM 直接加载，无 IDF bootloader）
 *
 * 内存窗口（依据 IDF 头文件，见 s31_regs.h 注释）：
 *   0x2F000000 .. 0x2F07AFC0   491456 B
 *
 * 布局（自底向上）：
 *   .boot / .text / .rodata / .data / .sdata / .bss / .sbss  ← 镜像
 *   heap                                     ← RT-Thread 小内存堆
 *   中断栈 (4KB)   __rt_rvstack = 栈顶        ← SW_handler 用 mscratch 切进来
 *   启动栈 (8KB)   __stack_top = 窗口顶        ← startup.S 用
 *
 * ⚠️ .text 段起点 64 字节对齐：CLIC 模式下 mtvec[31:6] 是处理函数基址，
 *    SW_handler 必须 64 字节对齐（MTVEC_MODE_CSR=3，riscv/csr_clic.h:22）。
 *
 * ⚠️ __global_pointer$：RISC-V 上小数据（.sdata/.sbss）走 gp 相对寻址，
 *    正常由 crt0 设置；我们 -nostartfiles 没有 crt0，必须在这里给出符号、
 *    在 startup.S 里 la gp 装载（否则访问小静态变量就是随机地址）。
 *===========================================================================*/

ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x2F000000, LENGTH = 0x7AFC0
}

SECTIONS
{
  .boot : {
    KEEP(*(.boot))
  } > RAM

  /* CLIC 陷阱入口跳板：必须落在 64 字节边界（mtvec[31:6] 是基址） */
  .clic_entry : {
    . = ALIGN(64);
    KEEP(*(.clic_entry))
  } > RAM

  /* .text.entry 放最前 → libcpu 的 SW_handler（跳板的目标） */
  .text : ALIGN(64) {
    KEEP(*(.text.entry))
    *(.text*)
  } > RAM

  .rodata : ALIGN(8) {
    *(.rodata*)
    *(.srodata*)
  } > RAM

  /* ---- 只读的"表"必须放在 .bss 之前 ----
   * 🚨 踩过的坑：这些表最初放在 .sbss 之后，而堆起点 __bss_end 只算到 .sbss 末尾
   *    → rt_system_heap_init() 直接把两张表当空闲内存写花
   *    → 遍历初始化表时跳到 0x8c（instruction access fault）。
   *    凡是有符号指向的区间，都必须落在 __heap_start 之前。 */

  /* RT-Thread 自动初始化表（INIT_BOARD_EXPORT / INIT_APP_EXPORT …）
   * 🚨 还必须 KEEP + SORT：
   *    rt_components_init() 是"按地址区间遍历"调用这些函数指针的
   *    （components.c:127 从 &__rt_init_rti_board_end 到 &__rt_init_rti_end —— 实测反汇编
   *      就是 0x2f00d370..0x2f00d38c，即 3..6 级；0/1 级那半张表要 rt_components_board_init()
   *      来遍历，而本 port 没有调用它，见 board.c 顶部注释），
   *    链接器看不到这种引用 → --gc-sections 会把它们当垃圾删掉。
   *    症状：所有 INIT_*_EXPORT 的函数一个都不执行（tick 不开、msh 不起来），
   *    而系统看上去"启动了"（版本号、main 都正常）。
   *    SORT 保证 .rti_fn.0 < .rti_fn.0.end < .rti_fn.1 < … < .rti_fn.6.end 的顺序。 */
  .rti_fn : ALIGN(4) {
    __rti_fn_start = .;
    KEEP(*(SORT(.rti_fn*)))
    __rti_fn_end = .;
  } > RAM

  /* msh 命令表（RT-Thread finsh 的 FSymTab，见 finsh.h:82 / shell.c:965） */
  .fsymtab : ALIGN(4) {
    __fsymtab_start = .;
    KEEP(*(FSymTab))
    __fsymtab_end = .;
  } > RAM

  .data : ALIGN(8) {
    *(.data*)
  } > RAM

  .sdata : ALIGN(8) {
    __sdata_start = .;
    *(.sdata*)
    __sdata_end = .;
  } > RAM

  .bss (NOLOAD) : ALIGN(8) {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } > RAM

  .sbss (NOLOAD) : ALIGN(8) {
    __sbss_start = .;
    *(.sbss*)        /* GCC 把小静态变量塞这里 */
    *(.scommon*)
    . = ALIGN(8);
    __sbss_end = .;
  } > RAM

  /* ---- 跨复位保留区（bootROM 不清 SRAM，startup.S 只清 .bss+.sbss）----
   * 用途：提频守卫的哨兵 —— "上次提频后没跑到确认"要能在下次开机被看见。
   * 必须放在 __bss_end 之后（否则会被清零）、__heap_start 之前（否则会被堆写花）。*/
  .noinit (NOLOAD) : ALIGN(8) {
    __noinit_start = .;
    KEEP(*(.noinit))
    KEEP(*(.noinit*))
    . = ALIGN(8);
    __noinit_end = .;
  } > RAM

  /* 清零范围 = .bss + .sbss（两者紧邻）。
   * 🚨 千万**不能**把 __bss_end 指到 .noinit 后面 —— startup.S 清的就是
   *    __bss_start..__bss_end 这一段，那样会把"跨复位保留区"一起清掉
   *    （踩过：提频守卫的哨兵写进去，复位后读回 0，守卫永远不触发）。
   *    堆的起点单独用 __noinit_end（见下方）。 */
  PROVIDE(__bss_end = __sbss_end);

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.riscv.attributes)
  }

  /* ---- gp（小数据指针）：RISC-V 标准算法，落在小数据区中部 ---- */
  PROVIDE(__global_pointer$ = MIN(__sdata_start + 0x800,
                                  MAX(__sdata_end, __sbss_end) - 0x800));

  /* newlib 的 sbrk 需要 end/_end（我们不实现 sbrk，符号给出即可） */
  PROVIDE(end  = __sbss_end);
  PROVIDE(_end = __sbss_end);

  /* ---- 栈与堆 ---- */
  __stack_top        = ORIGIN(RAM) + LENGTH(RAM);          /* 0x2F07AFC0 */
  __stack_bottom     = __stack_top - 0x2000;               /* 启动栈 8KB */
  __rt_rvstack       = __stack_bottom;                     /* 中断栈顶（mscratch 初值） */
  __irq_stack_bottom = __rt_rvstack - 0x1000;              /* 中断栈 4KB */
  __heap_start       = __noinit_end;                       /* 堆跳过 .noinit（它不能被写花）*/
  __heap_end         = __irq_stack_bottom;

  /*=========================================================================
   * ROM 函数 + 外设实例符号（2026-09-25 为移植 PSRAM 而从 boot_msc_s31 合并）
   *
   * 我们不用 IDF，但 `bsp/s31_psram.c` 直接调 IDF 的 LL 头（按字段名写寄存器，
   * 不手抄位号 —— 这是本项目付过学费的规矩）。那些头引用两类外部符号：
   *   ① 外设实例：`extern xxx_dev_t SPIMEM2;` —— IDF 靠 peripherals.ld 给地址
   *   ② ROM 函数：`Cache_Enable_L1_DCache()` 之类 —— IDF 靠 rom.ld 给地址
   * 地址都从 IDF 的 `components/soc/esp32s31/ld/` 与
   * `components/esp_rom/esp32s31/ld/` 抄来，改 IDF 版本时才需要复核。
   *=======================================================================*/
  PROVIDE(SPIMEM2       = 0x20502000);
  PROVIDE(SPIMEM3       = 0x20503000);
  PROVIDE(MSPI_IOMUX    = 0x20584000);
  PROVIDE(HP_SYSTEM     = 0x20586000);
  PROVIDE(HP_SYS_CLKRST = 0x20587000);
  PROVIDE(HP_ALIVE_SYS  = 0x20589000);
  PROVIDE(LP_SYS        = 0x20700000);
  PROVIDE(LP_AONCLKRST  = 0x20701000);
  PROVIDE(PMU           = 0x20704000);
  PROVIDE(MODEM_LPCON   = 0x2010F000);
  PROVIDE(EFUSE         = 0x20715000);
  PROVIDE(CACHE         = 0x2C000000);

  /* 读 PSRAM 芯片容量/mode register 用的 ROM 事务接口（rom/opi_flash.h）——
     psram_ctrlr_ll.h 的 psram_size_bytes() 就调这三个 */
  PROVIDE(esp_rom_spi_set_op_mode = 0x2f800124);
  PROVIDE(esp_rom_spi_cmd_config  = 0x2f80011c);
  PROVIDE(esp_rom_spi_cmd_start   = 0x2f800120);

  /* cache / MMU 的 ROM 入口（psram 初始化里用得到的那几个；其余按需再加）*/
  PROVIDE(Cache_Enable_L1_CORE0_ICache    = 0x2f800690);
  PROVIDE(Cache_Disable_L1_CORE0_ICache   = 0x2f80068c);
  PROVIDE(Cache_Enable_L1_DCache          = 0x2f8006b0);
  PROVIDE(Cache_Disable_L1_DCache         = 0x2f8006ac);
  PROVIDE(Cache_Suspend_L1_DCache         = 0x2f8006b4);
  PROVIDE(Cache_Resume_L1_DCache          = 0x2f8006b8);
  PROVIDE(Cache_Suspend_L1_CORE0_ICache   = 0x2f800694);
  PROVIDE(Cache_Resume_L1_CORE0_ICache    = 0x2f800698);
  PROVIDE(Cache_Invalidate_Addr           = 0x2f8005e8);
  PROVIDE(Cache_WriteBack_Addr            = 0x2f8005f0);
  PROVIDE(Cache_WriteBack_Invalidate_Addr = 0x2f8005f4);
  PROVIDE(Cache_Invalidate_All            = 0x2f8005f8);
  PROVIDE(Cache_PSRAM_MMU_Init            = 0x2f8006c0);
  PROVIDE(Cache_PSRAM_MMU_Set             = 0x2f8006cc);

  ASSERT(__noinit_end <= __heap_end, "BSS/NOINIT OVERLAPS HEAP: image too big for 0x2F000000..0x2F07AFC0")
  ASSERT(__heap_end >= __fsymtab_end, "HEAP OVERLAPS INIT/FINSH TABLES")
  ASSERT((s31_clic_entry & 0x3F) == 0, "s31_clic_entry MUST be 64-byte aligned for CLIC mtvec")
}
