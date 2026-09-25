#===========================================================================
# build_doc.ps1 -- 把 doc\*.md 渲染成 PDF（pandoc → HTML → Edge headless 打印）
#
#   pwsh -File tools\build_doc.ps1                          # 渲染 doc\ 下所有 md
#   pwsh -File tools\build_doc.ps1 -Name rt-thread-porting-tutorial
#
# 为什么走"HTML + 浏览器打印"而不是 LaTeX：
#   本机没有可用的 XeLaTeX/MiKTeX（装它要管理员权限、还容易卡在字体生成），
#   而 pandoc + Edge headless 这条链零依赖、中文/表格/代码块都很稳。
# 产物：doc\<名字>.html（中间件）、doc\<同名标题>.pdf（成品）
#===========================================================================
param(
    [string]$Name = '',           # 不带扩展名的 md 文件名；留空 = 全部
    [string]$Title = ''           # PDF 里的文档标题（留空则用 md 的 H1）
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Doc  = Join-Path $Root 'doc'
$Css  = Join-Path $Doc 'style.css'

# ---- 找 Edge（headless 打印 PDF）------------------------------------------
# ⚠️ 注意 ${env:ProgramFiles(x86)} 的花括号：PowerShell 里不带花括号的话
#    "$env:ProgramFiles(x86)" 会被当成 "$env:ProgramFiles" + 字面量 "(x86)"（踩过）
$Edge = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
    "$env:LOCALAPPDATA\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Edge) { Write-Host '找不到 msedge.exe（Edge）' -ForegroundColor Red; exit 1 }

$mdFiles = if ($Name) { @(Join-Path $Doc "$Name.md") }
           else { Get-ChildItem $Doc -Filter *.md | ForEach-Object { $_.FullName } }
if (-not $mdFiles) { Write-Host "doc\ 下没有 .md" -ForegroundColor Red; exit 1 }

foreach ($md in $mdFiles) {
    if (-not (Test-Path $md)) { Write-Host "缺文件: $md" -ForegroundColor Red; exit 1 }
    $base = [IO.Path]::GetFileNameWithoutExtension($md)
    $html = Join-Path $Doc "$base.html"

    # 标题：优先命令行给的 → 其次 md 的 YAML front-matter `title:` → 最后取第一个一级标题
    $docTitle = $Title
    if (-not $docTitle) {
        $docTitle = (Select-String -Path $md -Pattern '^title:\s*"?(.+?)"?\s*$' | Select-Object -First 1).Matches[0].Groups[1].Value
    }
    if (-not $docTitle) {
        $docTitle = (Select-String -Path $md -Pattern '^#\s+(.+)$' | Select-Object -First 1).Matches[0].Groups[1].Value
    }
    # 文件名里不能出现的字符换掉（Windows）
    $fileSafe = ($docTitle -replace '[\\/:*?"<>|]', '_')

    Write-Host "== pandoc: $base.md → $base.html" -ForegroundColor Cyan
    & pandoc $md -f markdown -t html5 --standalone --toc --toc-depth=2 `
        --metadata "title=$docTitle" --css style.css -o $html
    if ($LASTEXITCODE -ne 0) { Write-Host 'pandoc 失败' -ForegroundColor Red; exit 1 }

    $pdf  = Join-Path $Doc "$fileSafe.pdf"
    Remove-Item $pdf -Force -ErrorAction SilentlyContinue
    $prof = Join-Path $env:TEMP ("edgepdf_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $url  = 'file:///' + ($html -replace '\\', '/')
    Write-Host "== Edge headless → $fileSafe.pdf" -ForegroundColor Cyan
    # ⚠️ 用 `&` 直接调（阻塞），**别用 Start-Process -Wait**：实测后者在脚本里会静默不产文件，
    #    而直接调用会打印 "N bytes written to file ..."（拿来判断成功最方便）
    & $Edge --headless --disable-gpu --no-pdf-header-footer `
        "--user-data-dir=$prof" --virtual-time-budget=20000 `
        "--print-to-pdf=$pdf" $url 2>&1 | Select-String -Pattern 'bytes written|ERROR:.*pdf|failed' | Select-Object -First 3

    # 偶尔要过一会儿才落盘：轮询等它出现
    for ($i = 0; $i -lt 40 -and -not (Test-Path $pdf); $i++) { Start-Sleep -Milliseconds 250 }

    if (Test-Path $pdf) {
        Write-Host ("   OK: {0}  ({1:N0} KB)" -f $pdf, ((Get-Item $pdf).Length / 1KB)) -ForegroundColor Green
    } else {
        Write-Host '   没生成 PDF（Edge 静默失败，换个 user-data-dir 再试）' -ForegroundColor Red
    }
}
