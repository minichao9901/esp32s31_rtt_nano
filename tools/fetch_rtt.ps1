#===========================================================================
# fetch_rtt.ps1 -- 取 RT-Thread 源码（只拿我们要的部分，供 rtt_nano_s31 使用）
#
#   pwsh -File tools\fetch_rtt.ps1                 # 按 rt-thread.pin 里钉住的版本取（推荐）
#   pwsh -File tools\fetch_rtt.ps1 -Latest         # 取最新 v5.x tag（升级用，取完记得改 pin）
#   pwsh -File tools\fetch_rtt.ps1 -Tag v5.1.0     # 指定 tag
#   pwsh -File tools\fetch_rtt.ps1 -Force          # 已存在也重取
#   pwsh -File tools\fetch_rtt.ps1 -Mirror https://github.com/RT-Thread/rt-thread.git
#
# 为什么用稀疏检出：rt-thread 全仓库带几百个 BSP，几百 MB；我们只要
#   src/ include/ libcpu/(riscv) components/finsh/ components/drivers/
#   —— 内核 + msh + 设备框架（pin/spi/i2c）足矣。
# ⚠️ 这份清单要和 tools/build.ps1 的源码表对得上：那边用了
#   components/drivers/{core,pin,spi,i2c} 里的 6 个 .c，所以 drivers 必须整目录拿。
#   （2026-09-25 踩过：清单漏了 drivers，新 clone 下来编到 rtdevice.h 就断，
#     而本机因为早期手工补过目录所以一直没暴露。）
#
# ★ 版本可复现：`rt-thread.pin`（**进库**）里钉着"本工程验证过的 tag + commit"，
#   默认就按它取 —— 否则明天上游发 v5.3 你 clone 下来编不过，都不知道该怪谁。
#   实际取到的版本另记一份到 `rt-thread.commit`（不进库，只是本机记录）。
#
# 镜像：默认 gitee（国内快）；海外或 gitee 挂了就 -Mirror 换 GitHub。
#
# 目录 `rt-thread/` 不进 git（见 .gitignore）：它是第三方源码，
# 由本脚本按钉住的版本拉取，和 managed_components 一个思路。
#===========================================================================

param(
    [string]$Tag = '',
    [string]$Mirror = 'https://gitee.com/rtthread/rt-thread.git',
    [switch]$Latest,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Dest = Join-Path $Root 'rt-thread'
$Pin  = Join-Path $Root 'rt-thread.pin'
$Sparse = @('src', 'include', 'libcpu', 'components/finsh', 'components/drivers')

# ---- 定版本：-Tag > rt-thread.pin > 最新 v5.x -----------------------------
$PinTag = ''
$PinCommit = ''
if (Test-Path $Pin) {
    $pinLine = (Get-Content $Pin -Raw).Trim() -split '\s+'
    $PinTag    = $pinLine[0]
    $PinCommit = if ($pinLine.Count -gt 1) { $pinLine[1] } else { '' }
}

if ((Test-Path $Dest) -and -not $Force) {
    Write-Host "already present: $Dest  (use -Force to refetch)"
    if (Test-Path (Join-Path $Root 'rt-thread.commit')) {
        Write-Host ("本地实际版本: " + (Get-Content (Join-Path $Root 'rt-thread.commit') -Raw).Trim())
    }
    if ($PinTag) { Write-Host "pin (进库、验证过的版本): $PinTag $PinCommit" }
    exit 0
}
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }

if ($Tag -eq '') {
    if ((-not $Latest) -and $PinTag) {
        $Tag = $PinTag
        Write-Host "=== 按 rt-thread.pin 取：$Tag（要最新版用 -Latest）==="
    } else {
        Write-Host "=== 查询最新 v5.x tag ==="
        $tags = & git ls-remote --tags --refs $Mirror 2>$null |
                ForEach-Object { ($_ -split '/')[-1] } |
                Where-Object { $_ -match '^v5\.' } |
                Sort-Object { [version]($_ -replace '^v', '') }
        if (-not $tags) { throw "拿不到 tag 列表（网络？镜像地址：$Mirror）" }
        $Tag = $tags[-1]
    }
}
Write-Host "target tag : $Tag"

# ---- 优先：partial + sparse（快很多）--------------------------------------
$ok = $false
Write-Host "=== git clone --filter=blob:none --sparse ==="
& git clone --depth 1 --branch $Tag --filter=blob:none --sparse $Mirror $Dest
if ($LASTEXITCODE -eq 0) {
    Push-Location $Dest
    & git sparse-checkout set @Sparse
    $ok = ($LASTEXITCODE -eq 0)
    Pop-Location
}

# ---- 回退：完整 depth-1 clone（镜像不支持 partial clone 时）----------------
if (-not $ok) {
    Write-Host "=== 回退：普通 --depth 1 clone ===" -ForegroundColor Yellow
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    & git clone --depth 1 --branch $Tag $Mirror $Dest
    if ($LASTEXITCODE -ne 0) { throw 'clone 失败' }
}

# ---- 记录 commit -----------------------------------------------------------
$commit = (& git -C $Dest rev-parse HEAD).Trim()
"$Tag $commit" | Set-Content -Path (Join-Path $Root 'rt-thread.commit') -Encoding ASCII
Write-Host ""
Write-Host "=== done ==="
Write-Host "path   : $Dest"
Write-Host "tag    : $Tag"
Write-Host "commit : $commit"
if ($PinCommit) {
    if ($commit -eq $PinCommit) {
        Write-Host "pin    : 与 rt-thread.pin 一致 ✓" -ForegroundColor Green
    } else {
        Write-Host "pin    : ⚠️ 与 rt-thread.pin 不一致（pin=$PinCommit）" -ForegroundColor Yellow
        Write-Host "         如果这个版本验证过，把 rt-thread.pin 改成：$Tag $commit" -ForegroundColor Yellow
    }
}
Write-Host "sparse : $($Sparse -join ', ')"
Write-Host ""
Write-Host "内核文件数："
Get-ChildItem (Join-Path $Dest 'src') -Filter *.c | Measure-Object | ForEach-Object { Write-Host ("  src/*.c        : " + $_.Count) }
Get-ChildItem (Join-Path $Dest 'components\finsh') -Filter *.c | Measure-Object | ForEach-Object { Write-Host ("  finsh/*.c      : " + $_.Count) }
Get-ChildItem (Join-Path $Dest 'libcpu\risc-v') -Directory | ForEach-Object { Write-Host ("  libcpu/risc-v/" + $_.Name) }
