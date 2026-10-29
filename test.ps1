#requires -Version 5.1
<#
.SYNOPSIS
    portable-py 冒烟测试
#>
[CmdletBinding()]
param(
    [string]$Exe = (Join-Path $PSScriptRoot 'dist\portable-py.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:pass = 0
$script:fail = 0

function Check([string]$Name, [bool]$Cond, [string]$Detail = '') {
    if ($Cond) {
        Write-Host "PASS  $Name"
        $script:pass++
    } else {
        Write-Host "FAIL  $Name  $Detail" -ForegroundColor Red
        $script:fail++
    }
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到 $Exe，请先运行 .\build.ps1" }

$envDir  = Join-Path $env:TEMP 'portable_py_env'
$marker  = Join-Path $envDir '.portable_py_ok'
$pydll   = Join-Path $envDir 'python311.dll'
$sample  = Join-Path $PSScriptRoot (Join-Path 'build' 'sample.py')
$errFile = Join-Path $env:TEMP 'portable_py_test_stderr.txt'

"=== 0. 清理旧的解压环境 (保证一次冷启动) ==="
if (Test-Path -LiteralPath $envDir) { Remove-Item -Recurse -Force -LiteralPath $envDir }
"clean."

"=== 1. 冷启动 + 命令行透传 (核心需求) ==="
$t0 = Get-Date
$out = & $Exe -c "print('Hello Portable Python!')" 2>&1
$code = $LASTEXITCODE
$coldMs = [int]((Get-Date) - $t0).TotalMilliseconds
$all = ($out -join "`n")
Check 'cold run prints Hello' ($all -match 'Hello Portable Python!') "got: $all"
Check 'cold run exit code 0' ($code -eq 0) "got: $code"
"      冷启动(含解压)耗时 ${coldMs}ms"

"=== 2. 解压目录内容 ==="
Check 'python.exe extracted'   (Test-Path -LiteralPath (Join-Path $envDir 'python.exe'))
Check 'python311.dll extracted' (Test-Path -LiteralPath $pydll)
Check 'marker file written'    (Test-Path -LiteralPath $marker)
$size = (Get-ChildItem -Recurse -LiteralPath $envDir | Measure-Object -Property Length -Sum).Sum
"      $envDir 共 $([math]::Round($size / 1MB, 1)) MiB"

"=== 3. 解释器版本与路径 ==="
$out = & $Exe -c "import sys; print(sys.version); print(sys.executable)"
$ver = ($out -join "`n")
Check 'python 3.11.8'   ($ver -match '3\.11\.8') "got: $ver"
Check 'resides in TEMP' ($ver -match 'portable_py_env') "got: $ver"

"=== 4. 退出码透传 ==="
& $Exe -c "import sys; sys.exit(42)" 2>&1 | Out-Null
Check 'exit code 42 forwarded' ($LASTEXITCODE -eq 42) "got: $LASTEXITCODE"

"=== 5. 运行 .py 脚本文件 ==="
'print("script mode ok:", 6 * 7)' | Out-File -LiteralPath $sample -Encoding ascii
$out = & $Exe $sample
$scr = ($out -join "`n")
Check 'script runs' ($scr -match 'script mode ok: 42') "got: $scr"

"=== 6. stdin 模式 (-) ==="
$inp = 'print("stdin mode ok:", 2 ** 10)'
$out = $inp | & $Exe -
$std = ($out -join "`n")
Check 'stdin mode works' ($std -match 'stdin mode ok: 1024') "got: $std"

"=== 7. 标准库内置扩展可用 ==="
$out = & $Exe -c "import json, sqlite3, hashlib, ssl, zlib, unicodedata; print('stdlib ok')"
Check 'stdlib imports' (($out -join "`n") -match 'stdlib ok') "got: $out"

"=== 8. 二次运行走快速路径 (不再解压) ==="
& $Exe -c "print('warm run ok')" 2> $errFile | Out-Null
$err = if (Test-Path -LiteralPath $errFile) { Get-Content -LiteralPath $errFile -Raw } else { '' }
Check 'warm run silent (no unpacking)' (-not ($err -match 'unpacking')) "stderr: $err"
$t0 = Get-Date
& $Exe -c "pass" 2>&1 | Out-Null
$warmMs = [int]((Get-Date) - $t0).TotalMilliseconds
Check 'warm run ok' ($LASTEXITCODE -eq 0)
"      热启动耗时 ${warmMs}ms"

""
"RESULT: $($script:pass) passed, $($script:fail) failed"
if ($script:fail -gt 0) { exit 1 }
