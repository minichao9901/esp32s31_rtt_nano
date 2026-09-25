#===========================================================================
# gen_iomux_table.ps1 -- 从 IDF 头文件生成"引脚号 -> IO_MUX 寄存器偏移"表
#
#   pwsh -File tools\gen_iomux_table.ps1
#
# 为什么要有这个脚本：S31 的 IO_MUX 引脚寄存器**不是按引脚号连续排列**的
# （GPIO43 在 +0xac、GPIO61 在 +0xf4…），IDF 里是靠一张 63 项的查表
# （`components/soc/esp32s31/gpio_periph.c` 的 GPIO_PIN_MUX_REG）来定位。
# 手工抄 63 个偏移容易错一位，所以这里**直接从 io_mux_reg.h 解析**生成 C 头文件。
#===========================================================================

param(
    [string]$IdfPath = 'E:\esp32-idf\esp-idf',
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($OutFile -eq '') { $OutFile = Join-Path $Root 'bsp\s31_iomux_table.h' }

$hdr = Join-Path $IdfPath 'components\soc\esp32s31\register\soc\io_mux_reg.h'
if (-not (Test-Path $hdr)) { throw "找不到 $hdr（IDF 路径不对？）" }

$txt = Get-Content $hdr -Raw

# ① PERIPHS_IO_MUX_xxx -> 偏移
$offs = @{}
foreach ($m in [regex]::Matches($txt, '#define\s+(PERIPHS_IO_MUX_[A-Z0-9_]+)\s+\(REG_IO_MUX_BASE\s*\+\s*(0x[0-9a-fA-F]+)\)')) {
    $offs[$m.Groups[1].Value] = $m.Groups[2].Value
}

# ② IO_MUX_GPIO<n>_REG -> PERIPHS 名（从而拿到偏移）
$pinToOff = @{}
foreach ($m in [regex]::Matches($txt, '#define\s+IO_MUX_GPIO(\d+)_REG\s+(PERIPHS_IO_MUX_[A-Z0-9_]+)')) {
    $pin = [int]$m.Groups[1].Value
    $name = $m.Groups[2].Value
    if ($offs.ContainsKey($name)) { $pinToOff[$pin] = $offs[$name] }
}

if ($pinToOff.Count -eq 0) { throw '一个引脚都没解析出来，检查正则是否匹配当前 IDF 版本' }
$maxPin = ($pinToOff.Keys | Measure-Object -Maximum).Maximum
$count = $maxPin + 1

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('/* 自动生成，请勿手改 —— 由 tools/gen_iomux_table.ps1 从 ESP-IDF 的')
$lines.Add(' *   components/soc/esp32s31/register/soc/io_mux_reg.h 解析得到')
$lines.Add(' *   引脚号 -> IO_MUX 寄存器偏移（S31 的 IO_MUX 不按引脚号连续排列） */')
$lines.Add('#ifndef S31_IOMUX_TABLE_H')
$lines.Add('#define S31_IOMUX_TABLE_H')
$lines.Add('')
$lines.Add('#include <stdint.h>')
$lines.Add('')
$lines.Add('#define S31_GPIO_PIN_COUNT   ' + $count)
$lines.Add('')
$lines.Add('static const uint32_t s31_iomux_off[S31_GPIO_PIN_COUNT] = {')
for ($p = 0; $p -lt $count; $p++) {
    if ($pinToOff.ContainsKey($p)) {
        $lines.Add('    [' + $p.ToString().PadLeft(2) + '] = ' + $pinToOff[$p] + ',')
    } else {
        $lines.Add('    [' + $p.ToString().PadLeft(2) + '] = 0xFFFFFFFFu,   /* 该引脚无 IO_MUX */')
    }
}
$lines.Add('};')
$lines.Add('')
$lines.Add('#endif /* S31_IOMUX_TABLE_H */')

Set-Content -Path $OutFile -Value $lines -Encoding UTF8
Write-Host ("生成 " + $OutFile + " ：" + $pinToOff.Count + " 个引脚（0.." + $maxPin + "）")
Write-Host ("样例： GPIO43 -> " + $pinToOff[43] + " , GPIO61 -> " + $pinToOff[61])
