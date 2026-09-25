#===========================================================================
# sync_idf_headers.ps1 —— 把 rtt_nano_s31 需要的 IDF 头文件"冻结"进工程
#
#   pwsh -File tools\sync_idf_headers.ps1              # 重新同步（覆盖 bsp\idf_headers\）
#   pwsh -File tools\sync_idf_headers.ps1 -WhatIf      # 只看会拷哪些，不动文件
#   pwsh -File tools\sync_idf_headers.ps1 -IdfPath D:\esp-idf
#
# ★ 为什么需要这个脚本
#   `bsp/s31_psram.c` 直接用 IDF 的 LL 头按**字段名**写寄存器（不手抄位号 ——
#   本工作区因为手抄位号栽过两次）。但那几个头会拖出一整个传递闭包，
#   所以把它们拷进 bsp\idf_headers\，工程就不再依赖 IDF 源码树了。
#   （这份是从 boot_msc_s31/tools/ 抄来的，只改了工程名与 include 根的顺序。）
#
# ★ 怎么保证拷全了
#   不自己解析 #include（会漏掉条件编译、`#include "sibling.h"` 这类靠"同级目录"
#   解析的形式），而是让 **编译器自己说**：用 `gcc -M` 生成依赖表，
#   取其中所有落在 IDF 树里的头文件。编译器读到的就是全部。
#
# ★ 目录形状怎么定
#   每个头按"它相对**命中它的那个 include 根**的路径"存放 ——
#   因为 include 字符串（如 `hal/mmu_ll.h`）就是将来在那个扁平目录下的相对路径。
#   同级 include（`#include "soc_caps.h"`）靠"包含者所在目录"解析，
#   而目录形状保留了，所以照样能找到。
#===========================================================================

param(
    [string]$IdfPath = '',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Bsp  = Join-Path $Root 'bsp'
$App  = Join-Path $Root 'app'
$Dst  = Join-Path $Bsp  'idf_headers'

# ---- 1. 找 IDF 与工具链 ----------------------------------------------------
if (-not $IdfPath) {
    $IdfPath = if ($env:ESP32_S31_IDF_PATH) { $env:ESP32_S31_IDF_PATH } else { 'E:\esp32-idf\esp-idf' }
}
$IdfC = Join-Path $IdfPath 'components'
if (-not (Test-Path -LiteralPath $IdfC)) {
    Write-Host "找不到 ESP-IDF 源码树：$IdfC" -ForegroundColor Red
    Write-Host "用 -IdfPath 指定，或设环境变量 ESP32_S31_IDF_PATH" -ForegroundColor Yellow
    exit 1
}

$ToolDir = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf') -Directory |
           Sort-Object Name -Descending | Select-Object -First 1
$Gcc = Join-Path $ToolDir.FullName 'riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe'

# ---- 2. include 根（顺序必须和 build.ps1 一致：编译器取"第一个命中"的）------
$Roots = @(
    (Join-Path $IdfC 'esp_hal_mspi\esp32s31\include'),
    (Join-Path $IdfC 'hal\esp32s31\include'),
    (Join-Path $IdfC 'hal\include'),
    (Join-Path $IdfC 'hal\platform_port\include'),
    (Join-Path $IdfC 'soc\esp32s31\include'),
    (Join-Path $IdfC 'soc\esp32s31\register'),
    (Join-Path $IdfC 'soc\include'),
    (Join-Path $IdfC 'esp_common\include'),
    (Join-Path $IdfC 'esp_rom\esp32s31\include'),
    (Join-Path $IdfC 'esp_rom\esp32s31\include\esp32s31'),
    (Join-Path $IdfC 'esp_rom\include')
)

# ---- 3. 用 gcc -M 问编译器：到底读了哪些头 ---------------------------------
$src = @()
$src += Get-ChildItem $Bsp -Include *.c,*.S -File -Recurse | Select-Object -ExpandProperty FullName
$src += Get-ChildItem $App -Include *.c -File -Recurse | Select-Object -ExpandProperty FullName
$src  = $src | Where-Object { $_ -notmatch '\\idf_headers\\' }

$common = @(
    '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
    '-ffreestanding', '-nostdlib', '-nostartfiles',
    "-I$Bsp", "-I$App", "-I$Root"
)
$common += $Roots | ForEach-Object { "-I$_" }

# ★ RT-Thread 这边：include 路径必须和 build.ps1 完全一致，
#   否则 `gcc -M` 连 rtthread.h 都找不到（这份脚本是从 boot_msc_s31 抄来的，
#   那边是纯裸机、没有 RT-Thread，所以这一段是搬过来之后必须补的）。
#   ⚠️ 内核源码要 __RT_KERNEL_SOURCE__ 才看得到 rtsched.h（和 build.ps1 同一条规矩）——
#      对 `-M` 来说它不是必需的，但带上能和真实编译保持一致。
$Rtt = Join-Path $Root 'rt-thread'
$common += @(
    "-I$Rtt\include",
    "-I$Rtt\components\finsh",
    "-I$Rtt\components\drivers\include",
    "-I$Rtt\libcpu\risc-v\common",
    '-D__RTTHREAD__'
)

Write-Host "问编译器依赖（gcc -M）..." -ForegroundColor Cyan
$depOut = & $Gcc @common '-M' @src 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "-M 失败，原始输出：" -ForegroundColor Red
    $depOut | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
    exit 1
}

# 依赖行形如：  target.o: a.h b.h \
#                  c.h
# 直接把每行按空白切开，取以 .h 结尾的 token（尾部的续行符 "\" 也一并切掉）。
# ⚠️ 别用正则匹配"带盘符的路径" —— Windows 路径里全是反斜杠，很容易写错字符类。
$hdrs = @()
foreach ($line in $depOut) {
    foreach ($tok in ($line -split '\s+')) {
        $t = $tok.Trim().TrimEnd('\', '/').TrimEnd(':')
        if ($t -and $t -match '\.h$') { $hdrs += $t }
    }
}
$hdrs = $hdrs | Sort-Object -Unique
Write-Host ("  依赖表里共 {0} 个头（含工程内与工具链）" -f $hdrs.Count)

# 只留 IDF 树里的（工程自己的、以及工具链自带的系统头都不要）
$idfHdrs = @()
foreach ($h in $hdrs) {
    $full = $h
    if (-not [System.IO.Path]::IsPathRooted($full)) { $full = Join-Path $Root $h }
    if (-not (Test-Path -LiteralPath $full)) { continue }
    $full = (Resolve-Path -LiteralPath $full).Path
    if ($full.StartsWith($IdfC, [System.StringComparison]::OrdinalIgnoreCase)) {
        $idfHdrs += $full
    }
}
$idfHdrs = $idfHdrs | Sort-Object -Unique

if ($idfHdrs.Count -eq 0) {
    Write-Host "没从依赖表里认出任何 IDF 头 —— 是不是 IDF 路径不对？" -ForegroundColor Red
    exit 1
}

# ---- 4. 每个头算"相对命中根"的路径 ----------------------------------------
function Get-RelUnderRoot([string]$full) {
    $best = $null
    foreach ($r in $Roots) {
        $rp = (Resolve-Path -LiteralPath $r -ErrorAction SilentlyContinue)
        if (-not $rp) { continue }
        $rFull = $rp.Path
        if ($full.StartsWith($rFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            $rel = $full.Substring($rFull.Length).TrimStart('\', '/')
            # 取**最长**的匹配根（路径层级更深的那条），避免 "include" 前缀吃掉 "include\esp32s31"
            if (-not $best -or $rFull.Length -gt $best.RootLen) {
                $best = [pscustomobject]@{ Root = $rFull; RootLen = $rFull.Length; Rel = $rel }
            }
        }
    }
    return $best
}

$plan = @()
$conflict = @{}
foreach ($h in $idfHdrs) {
    $m = Get-RelUnderRoot $h
    if (-not $m) { continue }
    $rel = $m.Rel.Replace('/', '\')
    if ($conflict.ContainsKey($rel)) {
        if ($conflict[$rel] -ne $h) {
            Write-Host "⚠ 路径冲突：$rel" -ForegroundColor Yellow
            Write-Host "    已有 $($conflict[$rel])" -ForegroundColor Yellow
            Write-Host "    又来 $h" -ForegroundColor Yellow
        }
    } else {
        $conflict[$rel] = $h
        $plan += [pscustomobject]@{ Rel = $rel; Src = $h; Root = $m.Root }
    }
}

Write-Host ""
Write-Host ("要冻结 {0} 个头文件，落在 {1}" -f $plan.Count, $Dst) -ForegroundColor Cyan
$plan | Group-Object { ($_.Rel -split '\\')[0] } | Sort-Object Name | ForEach-Object {
    Write-Host ("  {0}\  {1} 个" -f $_.Name, $_.Count)
}

if ($WhatIf) { Write-Host "`n(-WhatIf：没有动任何文件)"; exit 0 }

# ---- 5. 拷过去 -------------------------------------------------------------
# ⚠️ **不要** `Remove-Item $Dst -Recurse` 整个目录再重建！
#    这个目录里除了 IDF 头，还有手写的 README.md（讲这堆文件是干嘛的）——
#    整目录删会把说明一起干掉（实测踩过：`make headers` 之后 README.md 没了）。
#    所以只删"由本脚本接管"的东西：所有 .h + 上次生成的 _SOURCE.txt。
if (-not (Test-Path -LiteralPath $Dst)) {
    New-Item -ItemType Directory -Force -Path $Dst | Out-Null
}
Get-ChildItem -LiteralPath $Dst -Recurse -File -Filter *.h -ErrorAction SilentlyContinue |
    Remove-Item -Force
$oldSrc = Join-Path $Dst '_SOURCE.txt'
if (Test-Path -LiteralPath $oldSrc) { Remove-Item -LiteralPath $oldSrc -Force }
# 顺手清掉因为删文件而变空的目录（git 本来也不跟踪空目录，纯粹为了干净）
Get-ChildItem -LiteralPath $Dst -Recurse -Directory -ErrorAction SilentlyContinue |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } |
    Remove-Item -Force -ErrorAction SilentlyContinue

$bytes = 0
foreach ($p in $plan) {
    $target = Join-Path $Dst $p.Rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $p.Src -Destination $target -Force
    $bytes += (Get-Item -LiteralPath $target).Length
}

# ---- 6. 留一份"从哪儿来的"--------------------------------------------------
$ver = ''
$verFile = Join-Path $IdfPath 'version.txt'
if (Test-Path -LiteralPath $verFile) {
    $ver = (Get-Content -LiteralPath $verFile -Raw).Trim()
}
if (-not $ver) {
    # version.txt 不一定有，退而求其次读 esp_idf_version.h
    $vh = Join-Path $IdfC 'esp_common\include\esp_idf_version.h'
    if (Test-Path -LiteralPath $vh) {
        $t = Get-Content -LiteralPath $vh -Raw
        $ma = [regex]::Match($t, '#define\s+ESP_IDF_VERSION_MAJOR\s+(\d+)').Groups[1].Value
        $mi = [regex]::Match($t, '#define\s+ESP_IDF_VERSION_MINOR\s+(\d+)').Groups[1].Value
        $pa = [regex]::Match($t, '#define\s+ESP_IDF_VERSION_PATCH\s+(\d+)').Groups[1].Value
        if ($ma) { $ver = "v$ma.$mi.$pa" }
    }
}
if (-not $ver) { $ver = 'unknown' }

$git = ''
try { $git = (& git -C $IdfPath rev-parse --short HEAD 2>$null) } catch { }
$gitDesc = ''
try { $gitDesc = (& git -C $IdfPath describe --tags --always 2>$null) } catch { }

$prov = @()
$prov += "来源 IDF 源码树 : $IdfPath"
$prov += "IDF 版本        : $ver"
if ($gitDesc) { $prov += "IDF git describe: $gitDesc" }
if ($git) { $prov += "IDF git commit  : $git" }
$prov += "同步时间        : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
$prov += "文件数          : $($plan.Count)"
$prov += "合计            : $([math]::Round($bytes/1KB,1)) KB"
$prov += ""
$prov += "include 根（顺序即编译器的搜索顺序）："
$prov += $Roots | ForEach-Object { "  $_" }
$prov += ""
$prov += "重新同步： pwsh -File tools\sync_idf_headers.ps1"
[System.IO.File]::WriteAllText((Join-Path $Dst '_SOURCE.txt'), ($prov -join "`r`n") + "`r`n",
                               (New-Object System.Text.UTF8Encoding($false)))

Write-Host ""
Write-Host ("完成：{0} 个文件 / {1} KB -> {2}" -f $plan.Count, [math]::Round($bytes/1KB,1), $Dst) -ForegroundColor Green
