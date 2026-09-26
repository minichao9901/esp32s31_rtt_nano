#===========================================================================
# make.ps1 -- Makefile 背后干活的（rtt_nano_s31）
#
#   pwsh -File tools\make.ps1 help
#   pwsh -File tools\make.ps1 build -Port COM43 [-Rebuild] [-Safe]
#   pwsh -File tools\make.ps1 flash -Port COM43 [-Seconds 8]
#   pwsh -File tools\make.ps1 msh   -Port COM43 -Msh "help|psram_info"
#
# 为什么逻辑都在这儿、Makefile 只做薄封装：
#   本工作区三个工程（boot_msc_s31 / s31_app / 本工程）一个套路 ——
#   Makefile 只负责把 PORT / 参数传进来，真正干活的是 build.ps1 + 那几个 python 工具。
#   （另外 PowerShell 里 `exit (函数调用)` 会把子进程输出整段吞掉，
#     必须函数里设 $script:Rc、最后再 exit $script:Rc —— 见文件末尾。）
#===========================================================================
param(
    [Parameter(Position = 0)][string]$Cmd = 'help',
    [string]$Port = '',
    [int]$Seconds = 0,          # 读串口秒数：0 = 一直读（Ctrl+C 退出）
    [string]$Msh = 'help',      # msh 命令，多条用 | 分隔
    [switch]$Rebuild,           # 全量重编
    [switch]$Safe,              # 40MHz 安全模式（320MHz 起不来时的救砖档）
    [switch]$Quiet              # tail/watch 的静默模式
)

$ErrorActionPreference = 'Stop'

$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Tools = Join-Path $Root 'tools'
$Build = Join-Path $Root 'build'
$PS    = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File')

# ---- 端口：命令行 > local.env.ps1 > 环境变量 > COM43 ----------------------
if ($Port -eq '') {
    $WsLocalEnv = Join-Path (Split-Path -Parent (Split-Path -Parent $Root)) 'local.env.ps1'
    if (Test-Path $WsLocalEnv) { . $WsLocalEnv }
    $Port = if ($env:ESP32_S31_PORT) { $env:ESP32_S31_PORT } else { 'COM43' }
}

# ---- python：优先用 IDF 的 python 环境（里面一定有 pyserial）-------------
function Get-Py {
    $p = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\python_env') -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending |
         ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
         Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $p) { $p = 'python' }        # 退而求其次：PATH 里的 python
    return $p
}

$script:Rc = 0

function Run-Pwsh {
    param([string]$Script, [string[]]$A)
    & pwsh @PS (Join-Path $Tools $Script) @A
    $script:Rc = $LASTEXITCODE
}

function Run-Py {
    param([string]$Script, [string[]]$A)
    & (Get-Py) (Join-Path $Tools $Script) @A
    $script:Rc = $LASTEXITCODE
}

switch ($Cmd) {

    'help' {
        Write-Host ''
        Write-Host '  rtt_nano_s31 —— RT-Thread Nano + msh on ESP32-S31（不依赖 idf.py）' -ForegroundColor Cyan
        Write-Host "  端口：$Port   （改法：`$env:ESP32_S31_PORT，或 make X PORT=COM7）" -ForegroundColor DarkGray
        Write-Host ''
        Write-Host '  make build        编译（增量）-> build\app.bin'
        Write-Host '  make rebuild      全量重编（改了 rtconfig.h / 加了源文件后用）'
        Write-Host '  make clean        删掉 build\'
        Write-Host '  make size         看各段大小 + CLIC 入口对齐检查'
        Write-Host '  make flash        编译 + 烧到 flash 0x2000 + 读 4 秒日志（走 esptool）'
        Write-Host '  make flash-ocd    同上但**走 JTAG**（不碰 USB CDC；esptool 烧完控制台会哑时用这条）'
        Write-Host '  make run          flash 之后一直看串口（Ctrl+C 退出；SECONDS=8 只看 8 秒）'
        Write-Host '  make monitor      只看串口（SECONDS=8 看一段；缺省一直看）'
        Write-Host '  make msh          敲 msh 命令并收响应：make msh MSH="help|psram_info|free"'
        Write-Host '  make rtt          **SEGGER RTT 控制台**（走 JTAG，不碰 USB-CDC）：'
        Write-Host '                      make rtt MSH="mount onboard0 / elm|ls /" SECONDS=6'
        Write-Host '  make wav          造测试 WAV 到 build\（WAV_ARGS="--rate 22050 --bits 16"）；'
        Write-Host '                      DRIVE=D 顺手拷到 MSC 盘上'
        Write-Host '  make watch        不复位地观察串口（板子在跑、你不想打断它时用）'
        Write-Host '  make tail         容错观察：设备掉了自动重开 —— 看"是不是在反复复位"'
        Write-Host '  make reset        手动拉 EN 复位一次，然后听几秒（板子"不理人"时用）'
        Write-Host '  make safe         40MHz 安全档（编译+烧录）：320MHz 起不来时的救砖档'
        Write-Host '  make headers      重新冻结 bsp\idf_headers\（IDF 改了 S31 寄存器定义时才要跑）'
        Write-Host '  make fetch        （重新）拉 RT-Thread 源码到 rt-thread\'
        Write-Host '  make doc          把 doc\*.md 渲染成 PDF'
        Write-Host ''
        Write-Host '  ★ 烧录占用 flash 0x2000（ROM 的二级镜像位置）—— 会和 IDF 的二级 bootloader 打架；' -ForegroundColor DarkGray
        Write-Host '    本工作区里 boot_msc_s31（MSC 拖拽 bootloader）也烧在那儿，来回切要重烧。' -ForegroundColor DarkGray
        Write-Host ''
    }

    'build' {
        $a = @('-BuildOnly', '-Port', $Port)
        if ($Rebuild) { $a += '-Rebuild' }
        if ($Safe)    { $a += '-Safe' }
        Run-Pwsh 'build.ps1' $a
    }

    'rebuild' {
        $a = @('-BuildOnly', '-Rebuild', '-Port', $Port)
        if ($Safe) { $a += '-Safe' }
        Run-Pwsh 'build.ps1' $a
    }

    'flash' {
        # 编译 + 烧录 + 读日志（读多久：SECONDS 给了就用它，否则 4 秒）
        $rs = if ($Seconds -gt 0) { $Seconds } else { 4 }
        $a = @('-Port', $Port, '-ReadSeconds', "$rs")
        if ($Rebuild) { $a += '-Rebuild' }
        if ($Safe)    { $a += '-Safe' }
        Run-Pwsh 'build.ps1' $a
    }

    # 走 JTAG 烧录（**不碰 USB CDC**）—— esptool 烧完控制台会哑时用这条
    'flash-ocd' {
        $a = @('-BuildOnly')
        if ($Rebuild) { $a += '-Rebuild' }
        if ($Safe)    { $a += '-Safe' }
        Run-Pwsh 'build.ps1' $a
        & pwsh @PS (Join-Path $Tools 'ocd_flash.ps1') @()
        $script:Rc = $LASTEXITCODE
        $rs = if ($Seconds -gt 0) { $Seconds } else { 4 }
        Run-Py 'read_port.py' @($Port, "$rs")
    }

    'run' {
        # flash 之后一直看（SECONDS=0 → read_port 的"一直读"约定）
        $a = @('-Port', $Port, '-ReadSeconds', "$Seconds")
        if ($Rebuild) { $a += '-Rebuild' }
        if ($Safe)    { $a += '-Safe' }
        Run-Pwsh 'build.ps1' $a
    }

    'monitor' {
        Run-Py 'read_port.py' @($Port, "$Seconds")
    }

    'msh' {
        # 敲命令：msh.py 会先等固件起来（开端口本身会复位芯片）
        Run-Py 'msh.py' @($Port, $Msh, "$(if ($Seconds -gt 0) { $Seconds } else { 6 })")
    }

    'rtt' {
        # SEGGER RTT 控制台（走 JTAG，不碰 USB-CDC）：MSH= 要敲的命令，SECONDS= 听完多久
        # 🚨 加 --reset：开 OpenOCD 不保证复位芯片，上一轮的挂载状态会留着
        $a = @('--reset')
        if ($Msh) { $a += $Msh }
        $a += "$(if ($Seconds -gt 0) { $Seconds } else { 6 })"
        Run-Py 'rtt.py' $a
    }

    'wav' {
        # 造一个测试 WAV 到 build\（默认 16kHz/8bit/小星星）；
        #   加参数：make wav WAV_ARGS="--rate 22050 --bits 16 --tune sweep"
        #   顺手拷到 MSC 盘：make wav DRIVE=D   （板子上先 `msc start`）
        $out = 'build\test.wav'
        $a = @($out)
        if ($env:WAV_ARGS) { $a += ($env:WAV_ARGS -split ' ') }
        Run-Py 'make_test_wav.py' $a
        if ($env:DRIVE) {
            $dst = "{0}:\{1}" -f $env:DRIVE, (Split-Path $out -Leaf)
            Copy-Item $out $dst -Force
            Write-VolumeCache -DriveLetter $env:DRIVE -ErrorAction SilentlyContinue
            Write-Host "已拷到 $dst —— PC 上先安全弹出，再在板子上 msc stop / mount onboard0 / elm" -ForegroundColor Green
        }
    }

    'watch' {
        $a = @($Port, "$(if ($Seconds -gt 0) { $Seconds } else { 20 })")
        if ($Quiet) { $a += '--quiet' }
        Run-Py 'usj_watch.py' $a
    }

    'tail' {
        $a = @($Port, "$(if ($Seconds -gt 0) { $Seconds } else { 20 })")
        if ($Quiet) { $a += '--quiet' }
        Run-Py 'tail_port.py' $a
    }

    'reset' {
        Run-Py 'reset_probe.py' @($Port, "$(if ($Seconds -gt 0) { $Seconds } else { 4 })")
    }

    'safe' {
        # 救砖档：完全不动时钟树（XTAL 40MHz），编译+烧录
        $rs = if ($Seconds -gt 0) { $Seconds } else { 4 }
        Run-Pwsh 'build.ps1' @('-Port', $Port, '-ReadSeconds', "$rs", '-Safe', '-Rebuild')
    }

    'size' {
        $elf = Join-Path $Build 'app.elf'
        if (-not (Test-Path $elf)) { Write-Host '还没有 build\app.elf —— 先 make build' -ForegroundColor Red; $script:Rc = 1; break }
        $ToolDir = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
                   Sort-Object Name -Descending | Select-Object -First 1
        $Bin = Join-Path $ToolDir.FullName 'riscv32-esp-elf\bin'
        & (Join-Path $Bin 'riscv32-esp-elf-size.exe') $elf
        $script:Rc = $LASTEXITCODE
        Write-Host ''
        & (Join-Path $Bin 'riscv32-esp-elf-nm.exe') $elf | Select-String ' s31_clic_entry$' | ForEach-Object {
            $addr = [Convert]::ToUInt32(($_.ToString().Trim() -split '\s+')[0], 16)
            Write-Host ("CLIC entry @ 0x{0:X8}   64B 对齐: {1}" -f $addr, (($addr % 64) -eq 0))
        }
        $bin = Join-Path $Build 'app.bin'
        if (Test-Path $bin) { Write-Host ("app.bin : {0} bytes" -f (Get-Item $bin).Length) }
    }

    'clean' {
        if (Test-Path $Build) { Remove-Item -Recurse -Force $Build }
        Write-Host 'build\ 已删除'
    }

    'headers' {
        Run-Pwsh 'sync_idf_headers.ps1' @()
    }

    'fetch' {
        Run-Pwsh 'fetch_rtt.ps1' @()
    }

    'doc' {
        Run-Pwsh 'build_doc.ps1' @()
    }

    default {
        Write-Host "未知目标：$Cmd —— 跑 make help 看有哪些" -ForegroundColor Red
        $script:Rc = 1
    }
}

# ⚠️ 这里必须是 `exit $script:Rc`，不能写 `exit (Run-Pwsh ...)`：
#    后者会把子进程（pwsh/python/gcc）的输出整段吞掉（本工作区踩过）。
exit $script:Rc
