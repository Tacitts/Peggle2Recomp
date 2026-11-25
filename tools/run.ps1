# Regenerate import stubs, build the runtime, run it for a limited time, show the log tail.
# Usage: powershell -File tools\run.ps1 [-Seconds 30] [-Tail 40] [-NoBuild]
param([int]$Seconds = 30, [int]$Tail = 40, [switch]$NoBuild)

$root = Split-Path -Parent $PSScriptRoot
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
Set-Location $root

if (-not $NoBuild) {
    & "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -I tools\gen_imports.py .
    $env:BUILD_ARGS = ''
    $out = cmd /c "tools\build.cmd runtime build\runtime" 2>&1
    $errs = $out | Select-String -Pattern ' error|FAILED|undefined symbol'
    if ($LASTEXITCODE -ne 0) { $errs | Select-Object -First 20; Write-Output "BUILD FAILED"; exit 1 }
}

$runDir = Join-Path $root 'build\runtime'
Set-Location $runDir
$p = Start-Process -FilePath (Join-Path $runDir 'Peggle2.exe') -ArgumentList "`"$root\game`"" -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $runDir 'run_stdout.txt') -RedirectStandardError (Join-Path $runDir 'run_stderr.txt')
$null = $p.Handle   # cache the handle so ExitCode is available after exit (PowerShell 5.1 quirk)
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); Write-Output "STILL RUNNING after $Seconds s (killed)" }
else { $p.WaitForExit(); Write-Output ("exit code: {0} (0x{0:X8})" -f $p.ExitCode) }
$stderr = Get-Content (Join-Path $runDir 'run_stderr.txt') -ErrorAction SilentlyContinue
if ($stderr) { Write-Output "stderr:"; $stderr | Select-Object -Last 10 }
$log = Get-Content (Join-Path $runDir 'peggle2_runtime.log')
Write-Output "log lines: $($log.Count)"
$log | Where-Object { $_ -notmatch '^\[imports\] variable' } | Select-Object -Last $Tail
