#requires -Version 5.1
<#
.SYNOPSIS
    把 launcher.exe 与官方嵌入式 Python 合并成单文件 portable-py.exe。

.DESCRIPTION
    打包分四步:
      1. 校验原材料 (缺 python-embed.zip 时自动从官网/镜像下载)。
      2. 把内嵌包解包到 build\stage, 再重打成“零压缩 (STORED)”的 payload.zip
         —— launcher 只实现 STORED 解压, 用体积换取零外部依赖。
      3. dist\portable-py.exe = launcher.exe + payload.zip + 20 字节 footer。
      4. 回读 footer 做完整性校验。

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Version 3.12.2
#>
[CmdletBinding()]
param(
    [string]$Version     = '3.11.8',
    [string]$EmbeddedZip = '',
    [string]$Launcher    = (Join-Path $PSScriptRoot 'build\launcher.exe'),
    [string]$OutFile     = (Join-Path $PSScriptRoot 'dist\portable-py.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$buildDir   = Join-Path $PSScriptRoot 'build'
$vendorDir  = Join-Path $PSScriptRoot 'vendor'
$stageDir   = Join-Path $buildDir 'stage'
$payloadZip = Join-Path $buildDir 'payload.zip'

function Get-EmbeddedPythonZip {
    param([string]$Ver, [string]$Vendor)
    $name = "python-$Ver-embed-amd64.zip"
    $dst  = Join-Path $Vendor $name
    if (Test-Path -LiteralPath $dst) { return $dst }

    New-Item -ItemType Directory -Force -Path $Vendor | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $mirrors = @(
        "https://www.python.org/ftp/python/$Ver/$name",
        "https://mirrors.huaweicloud.com/python/$Ver/$name",
        "https://registry.npmmirror.com/-/binary/python/$Ver/$name",
        "https://cdn.npmmirror.com/binaries/python/$Ver/$name"
    )
    foreach ($u in $mirrors) {
        Write-Host "  downloading $u"
        try {
            Invoke-WebRequest -Uri $u -OutFile $dst -UseBasicParsing -TimeoutSec 180
            $b = New-Object byte[] 4
            $fs = [System.IO.File]::OpenRead($dst)
            try { $null = $fs.Read($b, 0, 4) } finally { $fs.Dispose() }
            if ($b[0] -eq 0x50 -and $b[1] -eq 0x4B) { return $dst }
            Remove-Item -LiteralPath $dst -Force
        } catch {
            Write-Warning "  failed: $($_.Exception.Message)"
        }
    }
    throw "无法下载 $name，请手动放到 $Vendor 后重试"
}

# ---------------------------------------------------------------- 0. 校验原材料

if (-not $EmbeddedZip) { $EmbeddedZip = Get-EmbeddedPythonZip -Ver $Version -Vendor $vendorDir }
if (-not (Test-Path -LiteralPath $Launcher)) {
    throw "缺少 launcher: $Launcher。先编译: gcc -O2 -s -Wall -municode -static -o build\launcher.exe src\launcher.c"
}
if (-not (Test-Path -LiteralPath $EmbeddedZip)) {
    throw "缺少嵌入式 Python 包: $EmbeddedZip"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

# ---------------------------------------------------------------- 1. 解包 staged

Write-Host '[1/4] 展开嵌入式 Python 包'
Remove-Item -Recurse -Force -Path $stageDir -ErrorAction SilentlyContinue
Expand-Archive -Path $EmbeddedZip -DestinationPath $stageDir -Force
# 官方 embed zip 没有外层目录, python.exe 直接落在压缩包根; 若有外层目录则取它
if (Test-Path -LiteralPath (Join-Path $stageDir 'python.exe')) {
    $pyDir = $stageDir
} else {
    $root = Get-ChildItem -LiteralPath $stageDir -Directory | Select-Object -First 1
    if (-not $root) { throw "意外的压缩包目录结构: $stageDir" }
    $pyDir = $root.FullName
}
$rawBytes = (Get-ChildItem -Recurse -LiteralPath $pyDir | Measure-Object -Property Length -Sum).Sum
Write-Host "      展开后 $([math]::Round($rawBytes / 1MB, 1)) MiB -> $pyDir"

# ---------------------------------------------------------------- 2. 重打 STORED

Write-Host '[2/4] 重打为零压缩 (STORED) payload'
if (Test-Path -LiteralPath $payloadZip) { Remove-Item -LiteralPath $payloadZip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $pyDir, $payloadZip, [System.IO.Compression.CompressionLevel]::NoCompression, $false)

$z = [System.IO.Compression.ZipFile]::OpenRead($payloadZip)
try {
    $compressed = @($z.Entries | Where-Object { $_.CompressedLength -ne $_.Length })
    $entryCount = $z.Entries.Count
} finally { $z.Dispose() }
if ($compressed.Count -gt 0) {
    throw "payload 含压缩条目: $($compressed.FullName -join ', ')"
}
$payloadLen = (Get-Item -LiteralPath $payloadZip).Length
Write-Host "      $entryCount 个条目, $([math]::Round($payloadLen / 1MB, 1)) MiB, 全部 STORED"

# ---------------------------------------------------------------- 3. 拼接单文件

Write-Host '[3/4] 拼接 launcher + payload + footer -> portable-py.exe'
$distDir = Split-Path -Parent $OutFile
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Copy-Item -LiteralPath $Launcher -Destination $OutFile -Force
$launcherLen = (Get-Item -LiteralPath $Launcher).Length

$zin = [System.IO.File]::OpenRead($payloadZip)
$zout = [System.IO.File]::Open($OutFile, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
try { $zin.CopyTo($zout) } finally { $zin.Dispose(); $zout.Dispose() }

# footer: 'PPY1' | uint64 launcherLen | uint64 payloadLen  (小端)
$footer = New-Object byte[] 20
[Array]::Copy([BitConverter]::GetBytes([uint32]0x31595050), 0, $footer, 0, 4)
[Array]::Copy([BitConverter]::GetBytes([uint64]$launcherLen),  0, $footer, 4, 8)
[Array]::Copy([BitConverter]::GetBytes([uint64]$payloadLen), 0, $footer, 12, 8)
$fout = [System.IO.File]::Open($OutFile, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
try { $fout.Write($footer, 0, 20) } finally { $fout.Dispose() }

# ---------------------------------------------------------------- 4. 回读校验

Write-Host '[4/4] 校验产物 footer'
$outLen  = (Get-Item -LiteralPath $OutFile).Length
$check   = New-Object byte[] 20
$cin = [System.IO.File]::OpenRead($OutFile)
try {
    $null = $cin.Seek($outLen - 20, [System.IO.SeekOrigin]::Begin)
    $null = $cin.Read($check, 0, 20)
} finally { $cin.Dispose() }

$magic = [System.Text.Encoding]::ASCII.GetString($check, 0, 4)
$off   = [BitConverter]::ToUInt64($check, 4)
$len   = [BitConverter]::ToUInt64($check, 12)
if ($magic -ne 'PPY1' -or $off -ne $launcherLen -or $len -ne $payloadLen -or ($off + $len + 20) -ne $outLen) {
    throw "footer 校验失败: magic=$magic off=$off len=$len total=$outLen"
}

Write-Host ''
Write-Host "产物     : $OutFile"
Write-Host "总大小   : $([math]::Round($outLen / 1MB, 2)) MiB ($outLen bytes)"
Write-Host "  launcher: $launcherLen bytes"
Write-Host "  payload : $payloadLen bytes (STORED zip)"
Write-Host '打包完成。'
