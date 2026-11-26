# Soak test: run the game repeatedly and summarise crashes.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\soak.ps1 [-Runs 10] [-Seconds 60] [-AutoPress 1|A]
#   -AutoPress 1  pulses A and Start alternately (skips the intro, walks the menus)
#   -AutoPress A  pulses only A (plays a level without pausing)
# Each run's log is copied to build\soak\<timestamp>-run<N>.log. A run "survives" if it is still
# running when its time is up; otherwise its crash signature (code, faulting guest instruction,
# thread) is reported.
param([int]$Runs = 10, [int]$Seconds = 60, [string]$AutoPress = '1')

$root = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $root 'build\runtime'
$outDir = Join-Path $root 'build\soak'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$env:PEGGLE2_AUTOPRESS = $AutoPress

$results = @()
for ($i = 1; $i -le $Runs; $i++) {
    Set-Location $runDir
    $p = Start-Process -FilePath (Join-Path $runDir 'Peggle2.exe') -ArgumentList "`"$root\game`"" -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'soak_stdout.txt') -RedirectStandardError (Join-Path $runDir 'soak_stderr.txt')
    $null = $p.Handle
    $survived = -not $p.WaitForExit($Seconds * 1000)
    if ($survived) { $p.Kill(); $p.WaitForExit() }
    $log = Join-Path $runDir 'peggle2_runtime.log'
    $copy = Join-Path $outDir "$stamp-run$i.log"
    Copy-Item $log $copy -Force
    $lines = Get-Content $copy
    $crash = $lines | Select-String '==== CRASH|==== indirect call' | Select-Object -First 1
    $sig = ''
    if ($crash) {
        $at = ($lines | Select-String '^\s+#0 ' | Select-Object -First 1)
        $thr = ($lines | Select-String 'guest thread:' | Select-Object -First 1)
        $sig = ("{0} | {1} | {2}" -f $crash.Line.Trim(), ($at.Line -replace '^\s+',''), ($thr.Line -replace '^\s+','')) -replace '\s+', ' '
    }
    $swaps = ($lines | Select-String 'VdSwap #(\d+)' | Select-Object -Last 1)
    $frames = if ($swaps) { $swaps.Matches[0].Groups[1].Value } else { '0' }
    $state = if ($survived -and -not $crash) { 'ok' } elseif ($crash) { 'CRASH' } else { "exit $($p.ExitCode)" }
    $results += [pscustomobject]@{ Run = $i; Result = $state; LastSwap = $frames; Signature = $sig }
    Write-Output ("run {0,2}/{1}: {2,-6} last swap {3,6}  {4}" -f $i, $Runs, $state, $frames, $sig)
}
$crashes = ($results | Where-Object Result -ne 'ok').Count
Write-Output ("summary: {0} runs, {1} survived, {2} failed (AutoPress={3}, {4} s each); logs in {5}" -f $Runs, ($Runs - $crashes), $crashes, $AutoPress, $Seconds, $outDir)
$results | Where-Object Result -ne 'ok' | Group-Object Signature | Select-Object Count, Name | Format-Table -AutoSize -Wrap
