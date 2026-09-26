#===========================================================================
# build.ps1 -- 构建 / 烧录 / 验证 RT-Thread Nano (msh) on ESP32-S31
#
#   pwsh -File tools\build.ps1                       # 编译 + 烧录 + 读日志
#   pwsh -File tools\build.ps1 -BuildOnly            # 只编译
#   pwsh -File tools\build.ps1 -Msh "help|s31_info"  # 烧完自动敲 msh 命令
#   pwsh -File tools\build.ps1 -Rebuild              # 忽略增量，全量重编
#
# 全程不碰 idf.py / CMake / IDF 环境：
#   编译器  riscv32-esp-elf-gcc（.espressif\tools\riscv32-esp-elf\...）
#   烧录    python -m esptool（独立 esptool，原生支持 esp32s31）
#   镜像烧到 flash 0x2000，由芯片 bootROM 直接加载（不用 IDF bootloader）
#
# 为什么要逐文件编译：RT-Thread 的内核源必须带 __RT_KERNEL_SOURCE__ 才看得到
# rtsched.h 里的内部调度接口（rtsched.h:112），而 finsh/应用不能带。
#===========================================================================

param(
    [switch]$BuildOnly,
    [switch]$SkipFlash,
    [switch]$Rebuild,
    [switch]$Safe,
    [switch]$NoStub,          # 退回老的 --no-stub 烧录方式（默认不带，见下面烧录段的说明）
    [string]$Port = '',
    [string]$Msh = '',
    [int]$ReadSeconds = 4
)

$ErrorActionPreference = 'Stop'

$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Rtt   = Join-Path $Root 'rt-thread'
$Bsp   = Join-Path $Root 'bsp'
$App   = Join-Path $Root 'app'
$Build = Join-Path $Root 'build'
$ObjDir = Join-Path $Build 'obj'
$WsLocalEnv = Join-Path (Split-Path -Parent (Split-Path -Parent $Root)) 'local.env.ps1'

if (-not (Test-Path (Join-Path $Rtt 'src'))) {
    Write-Host "RT-Thread 源码不在 $Rtt，先跑：pwsh -File tools\fetch_rtt.ps1" -ForegroundColor Red
    exit 1
}

# ---- 工具链（绝对路径）----------------------------------------------------
$ToolDir = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
           Sort-Object Name -Descending | Select-Object -First 1
$BinDir  = Join-Path $ToolDir.FullName 'riscv32-esp-elf\bin'
$Gcc     = Join-Path $BinDir 'riscv32-esp-elf-gcc.exe'
$Size    = Join-Path $BinDir 'riscv32-esp-elf-size.exe'
$Nm      = Join-Path $BinDir 'riscv32-esp-elf-nm.exe'

$PyExe = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\python_env') -Directory |
         Sort-Object Name -Descending |
         ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
         Where-Object { Test-Path $_ } | Select-Object -First 1
# ⚠️ 没装 IDF 的机器上没有 ~\.espressif\python_env —— 那就用 PATH 里的 python
#    （`pip install esptool` 即可）。少了这一行会报一句很难懂的 "$PyExe : 无法将..."
if (-not $PyExe) { $PyExe = 'python' }

if ($Port -eq '') {
    if (Test-Path $WsLocalEnv) { . $WsLocalEnv }
    $Port = if ($env:ESP32_S31_PORT) { $env:ESP32_S31_PORT } else { 'COM43' }
}

if ($Rebuild -and (Test-Path $Build)) { Remove-Item -Recurse -Force $ObjDir -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# ---- -Safe 戳：增量编译**看不出** -DS31_CLK_TARGET_MHZ 的差异 ----------------
# 现象：`make safe`（40MHz）之后再来一发 `make build`，所有 .o 都比源码新 →
#       一个都不重编 → 你拿到的**还是 40MHz 版**（实测 app.bin 62304 vs 63792，
#       差点就这么烧上板）。所以把"上次是不是 Safe"记在 build\.safe 里，变了就全量重编。
$SafeStamp = Join-Path $Build '.safe'
$WantSafe  = if ($Safe) { '1' } else { '0' }
$HaveSafe  = if (Test-Path $SafeStamp) { (Get-Content $SafeStamp -Raw).Trim() } else { '' }
$ForceAll  = $false
if ($HaveSafe -ne $WantSafe) {
    if ($HaveSafe -ne '') {
        Write-Host "Safe 档变了（$HaveSafe -> $WantSafe）-> 全量重编" -ForegroundColor Yellow
    }
    $ForceAll = $true
    Set-Content -Path $SafeStamp -Value $WantSafe
}

# ---- 源码分组 --------------------------------------------------------------
# ① 内核：要 __RT_KERNEL_SOURCE__（scheduler_up.c / thread.c 等内部接口）
#    按 rtconfig.h 的选择剔除不要的文件（与 rt-thread/src/SConscript 同一套规则）
$kernelExclude = @(
    'cpu_mp.c', 'scheduler_mp.c',   # 非 SMP
    'slab.c',                        # 未开 RT_USING_SLAB
    'mempool.c',                     # 未开 RT_USING_MEMPOOL
    'memheap.c',                     # 未开 RT_USING_MEMHEAP
    'signal.c'                       # 未开 RT_USING_SIGNALS
)
$kernelSrc = Get-ChildItem (Join-Path $Rtt 'src') -Filter *.c |
             Where-Object { $kernelExclude -notcontains $_.Name } |
             Select-Object -ExpandProperty FullName

# ② 其余：klibc + libcpu 端口 + finsh + BSP + 应用
#    klibc 在 RT-Thread 5.x 里被拆到 src/klibc/（rt_memset/rt_memcpy/rt_vsnprintf/
#    rt_strerror 都在那儿），内核 glob 只取 src 顶层，所以这里显式列出。
$otherSrc = @(
    (Join-Path $Rtt 'src\klibc\kerrno.c'),
    (Join-Path $Rtt 'src\klibc\kstdio.c'),
    (Join-Path $Rtt 'src\klibc\kstring.c'),
    (Join-Path $Rtt 'src\klibc\rt_vsnprintf_tiny.c'),
    (Join-Path $Rtt 'src\klibc\rt_vsscanf.c'),
    (Join-Path $Rtt 'libcpu\risc-v\common\cpuport.c'),
    # trap_common.c 不编：它的 rt_hw_interrupt_* / rt_rv32_system_irq_handler 是弱符号，
    # 我们用自己的 bsp/trap_handler.c（多打 mcause/mepc/mtval，且异常后停机不刷屏）
    (Join-Path $Rtt 'libcpu\risc-v\common\context_gcc.S'),
    (Join-Path $Rtt 'libcpu\risc-v\common\interrupt_gcc.S')
)
$otherSrc += @('shell.c', 'msh.c', 'msh_parse.c', 'cmd.c') |
             ForEach-Object { Join-Path $Rtt "components\finsh\$_" }
# ③ RT-Thread 设备框架 + 三个基础外设的核心（经典接口，不走 RT_USING_DM）
#    - core/device.c       设备框架本体
#    - pin/dev_pin.c       标准 rt_pin_* + 自带 `pin` msh 命令
#    - spi/dev_spi_core.c  总线/设备模型；dev_spi.c 是挂 spi flash 那套
#    - spi/dev_qspi_core.c QSPI（RT_USING_QSPI）
#    - i2c/dev_i2c_core.c  rt_i2c_transfer；dev_i2c_dev.c 是 i2c 从设备那套
$otherSrc += @(
    (Join-Path $Rtt 'components\drivers\core\device.c'),
    (Join-Path $Rtt 'components\drivers\pin\dev_pin.c'),
    (Join-Path $Rtt 'components\drivers\spi\dev_spi_core.c'),
    (Join-Path $Rtt 'components\drivers\spi\dev_spi.c'),
    (Join-Path $Rtt 'components\drivers\spi\dev_qspi_core.c'),
    (Join-Path $Rtt 'components\drivers\i2c\dev_i2c_core.c'),
    (Join-Path $Rtt 'components\drivers\i2c\dev_i2c_dev.c')
)

# ④ SFUD：**RT-Thread 官方那套**，直接从 rt-thread 源码树编，不拷进 bsp/、不改一个字
#    - sfud/src/sfud*.c          SFUD 引擎本体
#    - dev_spi_flash_sfud.c      官方移植层（wr/lock/unlock 钩子）+ `sf` 命令 + **块设备注册**
#    bsp/drv_spi_flash.c 里调一次 rt_sfud_flash_probe() 就带起来；开关在 bsp/rtconfig.h。
$otherSrc += @(
    (Join-Path $Rtt 'components\drivers\spi\sfud\src\sfud.c'),
    (Join-Path $Rtt 'components\drivers\spi\sfud\src\sfud_sfdp.c'),
    (Join-Path $Rtt 'components\drivers\spi\dev_spi_flash_sfud.c')
)

$otherSrc += @('startup.S', 'trap_gcc.S', 'board.c', 'drv_usj.c', 'drv_usj_dev.c',
               'drv_systick.c', 'drv_clk.c', 'trap_handler.c', 'syscalls_stub.c') |
             ForEach-Object { Join-Path $Bsp $_ }
# 外设驱动：写好一个就自动参与编译（还在写的时候就跳过）
$otherSrc += @('drv_gpio.c', 'drv_spi.c', 'drv_i2c.c', 'drv_lcd_axs15352.c',
               'drv_spi_flash.c') |
             ForEach-Object { Join-Path $Bsp $_ } |
             Where-Object { Test-Path $_ }
$otherSrc += @('s31_psram.c') |
             ForEach-Object { Join-Path $Bsp $_ } |
             Where-Object { Test-Path $_ }
$otherSrc += (Join-Path $App 'main.c')

$inc = @(
    "-I$Bsp",
    # SFUD（官方组件）：sfud_def.h 里是 `#include <sfud_cfg.h>`（尖括号），
    # 所以官方那份 inc 目录必须进搜索路径；dev_spi_flash.h 在同级目录。
    "-I$Rtt\components\drivers\spi\sfud\inc",
    "-I$Rtt\components\drivers\spi",
    # ★ 冻结的 IDF 头（bsp/s31_psram.c 里的 LL 头靠它编过）——
    #   放最后，工程自己的同名头优先
    "-I$Bsp\idf_headers",
    "-I$Rtt\include",
    "-I$Rtt\components\finsh",
    "-I$Rtt\components\drivers\include",
    "-I$Rtt\libcpu\risc-v\common"
)

# -nostartfiles：不要工具链 crt0（启动是我们自己的 startup.S）
# 其余照常链 newlib/libgcc（内核要用 memcpy/memset/strlen 与 __udivdi3）
$Common = @(
    '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
    '-O2', '-g', '-nostartfiles',
    '-ffunction-sections', '-fdata-sections',
    '-Wall', '-Wno-unused-parameter', '-Wno-unused-variable',
    '-Wno-unused-but-set-variable', '-Wno-missing-prototypes',
    '-D__RTTHREAD__'
) + $inc

# -Safe：退回 POR 的 XTAL 40MHz（完全不动时钟树），用于"320MHz 起不来"时救砖
if ($Safe) {
    $Common += '-DS31_CLK_TARGET_MHZ=40'
    Write-Host "safe mode : CPU 目标频率 = 40MHz（XTAL，不碰 PLL）" -ForegroundColor Yellow
}

Write-Host "toolchain : $BinDir"
Write-Host "sources   : kernel $($kernelSrc.Count) + other $($otherSrc.Count)"

$objs = @()
$failed = $false

# 头文件依赖：这些一改，所有 .o 都必须重编（否则会拿旧配置的 .o 去链接，
# 症状是"明明改了 rtconfig.h 却报某某符号未定义"—— 踩过）
$GlobalHeaders = @(
    (Join-Path $Bsp 'rtconfig.h'),
    (Join-Path $Bsp 's31_regs.h'),
    # SFUD 的配置头（官方那份）：改了它（开/关 SFDP、型号表）必须全量重编，
    # 否则会拿旧配置的 .o 去链，症状是"明明打开了某个功能却没生效"（踩过）
    (Join-Path $Rtt 'components\drivers\spi\sfud\inc\sfud_cfg.h')
)
$NewestHeader = ($GlobalHeaders | Where-Object { Test-Path $_ } |
                 ForEach-Object { (Get-Item $_).LastWriteTime } |
                 Sort-Object -Descending | Select-Object -First 1)

function Compile-One {
    param([string]$File, [string[]]$Extra)
    $name = [System.IO.Path]::GetFileNameWithoutExtension($File)
    $obj  = Join-Path $ObjDir ($name + '.o')
    $script:objs += $obj
    if ((Test-Path $obj) -and (-not ($Rebuild -or $ForceAll))) {
        $objTime = (Get-Item $obj).LastWriteTime
        if ($objTime -gt (Get-Item $File).LastWriteTime -and
            ($null -eq $NewestHeader -or $objTime -gt $NewestHeader)) { return }
    }
    $args = $Common + $Extra + @('-c', $File, '-o', $obj)
    & $Gcc @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("COMPILE FAILED: " + $File) -ForegroundColor Red
        $script:failed = $true
    }
}

Push-Location $Build
try {
    foreach ($f in $kernelSrc) { Compile-One $f @('-D__RT_KERNEL_SOURCE__') }
    foreach ($f in $otherSrc)  { Compile-One $f @() }
    if ($failed) { exit 1 }

    # ---- 链接 --------------------------------------------------------------
    # --gc-sections：丢掉没引用到的段（interrupt_gcc.S 里那个用不上的
    # trap_entry 会引用未定义的 handle_trap，靠它掉掉；FSymTab/.text.entry
    # 在 linker.ld 里是 KEEP，不会被误删）
    # SW_handler 必须 64 字节对齐（CLIC 的 mtvec[31:6]），linker.ld 有 ASSERT 兜底
    & $Gcc @Common "-Wl,-Map=$Build\app.map" '-Wl,--no-warn-rwx-segments' '-Wl,--gc-sections' `
        '-T' (Join-Path $Bsp 'linker.ld') '-o' (Join-Path $Build 'app.elf') @objs
    if ($LASTEXITCODE -ne 0) { Write-Host 'LINK FAILED' -ForegroundColor Red; exit 1 }
    Write-Host '=== built app.elf ==='
    & $Size app.elf

    & $PyExe -m esptool --chip esp32s31 elf2image -fm dio -ff 80m -fs 16MB -o app.bin app.elf
    if ($LASTEXITCODE -ne 0) { Write-Host 'ELF2IMAGE FAILED' -ForegroundColor Red; exit 1 }
    Write-Host ("=== app.bin : {0} bytes ===" -f (Get-Item app.bin).Length)

    $entry = (& $Nm app.elf | Select-String ' s31_clic_entry$' | Select-Object -First 1)
    if ($entry) {
        $addr = [Convert]::ToUInt32($entry.ToString().Split(' ')[0], 16)
        Write-Host ("CLIC entry @ 0x{0:X8}   64B aligned: {1}" -f $addr, (($addr % 64) -eq 0))
    } else {
        Write-Host 'WARNING: s31_clic_entry not found in ELF' -ForegroundColor Yellow
    }
}
finally {
    Pop-Location
}

if ($BuildOnly -or $SkipFlash) { Write-Host 'done (no flash)'; exit 0 }

# ---- 烧录到 0x2000（bootROM 的二级镜像偏移）-------------------------------
# 🚨 **别再默认加 `--no-stub`**（2026-09-25 改）：本工程原来硬编码 `--no-stub`，
#    而 IDF 的 `idf.py flash` 默认是**带 stub** 的（`serial_ext.py:84` 只在用户
#    显式要求时才加 `--no-stub`）。差别很要命：
#      · 带 stub：esptool 先往芯片 RAM 传一个 flasher stub，烧完由 stub 做干净收尾；
#      · 不带 stub：直接用 ROM 的原语烧 —— **收尾时 USB-Serial/JTAG 会被留在
#        "上一次会话"的状态里**，现象就是"烧完控制台哑了"（串口 0 字节，
#        而 JTAG 采 PC 显示 app 一直在正常跑 idle/spinlock）。
#    另一个结构性差别：IDF 有二级 bootloader，它启动时会把 console（含 USB-Serial/JTAG）
#    重新初始化一遍；本工程没有 bootloader，ROM 直接跳进 app，没人替我们清那个残留。
#    真要退回老路子：`make flash NOSTUB=1`。
Push-Location $Build
try {
    $stubArg = if ($NoStub) { @('--no-stub') } else { @() }
    & $PyExe -m esptool --chip esp32s31 -p $Port -b 460800 @stubArg `
        --before default-reset --after hard-reset `
        write-flash -fm dio -ff 80m -fs 16MB 0x2000 app.bin
    if ($LASTEXITCODE -ne 0) { Write-Host 'FLASH FAILED' -ForegroundColor Red; exit 1 }
}
finally {
    Pop-Location
}

# ---- 读日志 / 敲 msh -------------------------------------------------------
Start-Sleep -Milliseconds 800
if ($Msh -ne '') {
    & $PyExe (Join-Path $Root 'tools\msh.py') $Port $Msh $ReadSeconds
} else {
    & $PyExe (Join-Path $Root 'tools\read_port.py') $Port $ReadSeconds
}
Write-Host 'done'
