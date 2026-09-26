#===========================================================================
# ocd_flash.ps1 —— 用 OpenOCD（JTAG）烧 build\app.bin 到 flash 0x2000
#
#   pwsh -File tools\ocd_flash.ps1                 # 烧 + 回读逐字节校验 + 复位运行
#   pwsh -File tools\ocd_flash.ps1 -NoVerify       # 只烧
#   pwsh -File tools\ocd_flash.ps1 -VerifyOnly     # 只回读校验（不烧）
#
# 为什么需要它（2026-09-25 定论）：
#   本板 CDC（USB-Serial/JTAG）有个反复出现的毛病 —— **esptool 烧完之后控制台就哑**：
#   串口 0 字节、而 JTAG 采 PC 显示 app 一直在正常跑（idle/spinlock）。
#   那是 esptool 进出下载模式时把 USB 设备块留在了"上一次会话"的状态里。
#   走 **JTAG** 烧录完全不碰那个块，所以烧完控制台照常活着 —— 这是目前最省事的解法。
#
# ⚠️ OpenOCD 自带的 `verify` 在 S31 上会**段错误**（exit 0xC0000005，日志停在
#    `** Verify Started **`），但**写入是成功的**。所以这里自己回读校验：
#    `flash read_bank` 把整段读回来跟 app.bin 逐字节比（实测 0 字节不符）。
# ⚠️ `program` 会警告 `Unknown magic number in partition table` —— 我们不用 IDF
#    分区表（镜像烧在 ROM 的二级镜像位置 0x2000），忽略即可。
#===========================================================================
param(
    [switch]$NoVerify,
    [switch]$VerifyOnly,
    [int]$Size = 0            # 回读长度；0 = 按 app.bin 向上取整到 4KB
)

$ErrorActionPreference = 'Stop'

$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Build = Join-Path $Root 'build'
$Bin   = Join-Path $Build 'app.bin'
$Flash = 0x2000

$Ocd = Get-ChildItem (Join-Path $env:USERPROFILE '.espressif\tools\openocd-esp32') -Directory -ErrorAction SilentlyContinue |
       Sort-Object Name -Descending | Select-Object -First 1 |
       ForEach-Object { Join-Path $_.FullName 'openocd-esp32\bin\openocd.exe' }
if (-not $Ocd -or -not (Test-Path $Ocd)) {
    Write-Host "找不到 openocd.exe（~\.espressif\tools\openocd-esp32\...）" -ForegroundColor Red
    exit 1
}
$Scripts = Join-Path (Split-Path (Split-Path $Ocd)) 'share\openocd\scripts'
$Cfg     = Join-Path $Scripts 'board\esp32s31-builtin.cfg'

if (-not (Test-Path $Bin)) {
    Write-Host "没有 $Bin —— 先 make build" -ForegroundColor Red
    exit 1
}
$BinLen = (Get-Item $Bin).Length
if ($Size -le 0) {
    $Size = [int](([math]::Ceiling($BinLen / 4096.0)) * 4096)
}

function Invoke-Ocd([string[]]$Cmds) {
    # ⚠️ 别把变量叫 $args —— 那是 PowerShell 的自动变量，会被覆盖出怪问题
    $ocdArgs = @('-s', $Scripts, '-f', $Cfg)
    foreach ($c in $Cmds) { $ocdArgs += @('-c', $c) }
    & $Ocd @ocdArgs *> (Join-Path $Build 'ocd.log')
    return $LASTEXITCODE
}

if (-not $VerifyOnly) {
    Write-Host ("[ocd] 烧录 {0} 字节 -> flash 0x{1:X}" -f $BinLen, $Flash) -ForegroundColor Cyan
    $rc = Invoke-Ocd @('init', 'reset halt',
                       ("program {0} 0x{1:X}" -f ($Bin -replace '\\','/'), $Flash),
                       'shutdown')
    $log = Get-Content (Join-Path $Build 'ocd.log') -Raw
    if ($log -notmatch 'Programming Finished') {
        Write-Host "[ocd] 烧录失败，日志尾部：" -ForegroundColor Red
        Get-Content (Join-Path $Build 'ocd.log') | Select-Object -Last 12
        exit 1
    }
    Write-Host "[ocd] 写入完成" -ForegroundColor Green
}

if (-not $NoVerify) {
    $rb = Join-Path $Build 'readback.bin'
    if (Test-Path $rb) { Remove-Item $rb -Force }
    Write-Host ("[ocd] 回读 {0} 字节校验 ..." -f $Size) -ForegroundColor Cyan
    # ⚠️ 路径要给绝对路径 + 正斜杠（OpenOCD 认不了反斜杠/相对路径）
    Invoke-Ocd @('init', 'reset halt',
                 ("flash read_bank 0 {0} 0x{1:X} {2}" -f ($rb -replace '\\','/'), $Flash, $Size),
                 'shutdown') | Out-Null
    if (-not (Test-Path $rb)) {
        Write-Host "[ocd] 回读失败，日志尾部：" -ForegroundColor Red
        Get-Content (Join-Path $Build 'ocd.log') | Select-Object -Last 12
        exit 1
    }
    $a = [System.IO.File]::ReadAllBytes($rb)
    $b = [System.IO.File]::ReadAllBytes($Bin)
    $bad = 0
    for ($i = 0; $i -lt $b.Length; $i++) { if ($a[$i] -ne $b[$i]) { $bad++ } }
    if ($bad -eq 0) {
        Write-Host ("[ocd] 校验通过：{0} 字节逐字节一致" -f $b.Length) -ForegroundColor Green
    } else {
        Write-Host ("[ocd] 校验失败：{0} 字节不符" -f $bad) -ForegroundColor Red
        exit 1
    }
}

Write-Host "[ocd] 复位运行" -ForegroundColor Cyan
Invoke-Ocd @('init', 'reset run', 'shutdown') | Out-Null
Write-Host "[ocd] done —— CDC 没被碰过，控制台应该照常活着" -ForegroundColor Green
exit 0
