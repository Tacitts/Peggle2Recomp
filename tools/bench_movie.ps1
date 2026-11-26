# Movie cadence benchmark: boot with no input and let the intro movie and title play, then count
# hitches on screen: new frames shown 3+ display refreshes after the previous one (DXGI frame
# statistics, the "[frames] on screen" lines every 10 s). The CPU-side "new frames shown" lines
# over-count: they include render-thread jitter that never reaches the screen.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench_movie.ps1 [-Runs 3] [-Seconds 32] [-Label name]
param([int]$Runs = 3, [int]$Seconds = 32, [string]$Label = '')

$root = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $root 'build\runtime'
Remove-Item Env:PEGGLE2_AUTOPRESS -ErrorAction SilentlyContinue
$env:PEGGLE2_NO_LAUNCHER = '1'
$all = @()
for ($i = 1; $i -le $Runs; $i++) {
    Set-Location $runDir
    $p = Start-Process -FilePath (Join-Path $runDir 'Peggle2.exe') -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'bench_stdout.txt') -RedirectStandardError (Join-Path $runDir 'bench_stderr.txt')
    $null = $p.Handle
    if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); $p.WaitForExit() }
    Start-Sleep -Milliseconds 300
    $windows = @()
    foreach ($l in Get-Content (Join-Path $runDir 'peggle2_runtime.log')) {
        if ($l -match '\[frames\] on screen: (\d+) frames.*hitches \(\d\+\) (\d+)') {
            $windows += ('{0} hitches in {1} frames' -f $Matches[2], $Matches[1])
            $all += [int]$Matches[2]
        }
    }
    Write-Output ("{0} run {1}: per 10 s window: {2}" -f $Label, $i, ($windows -join ', '))
}
Remove-Item Env:PEGGLE2_NO_LAUNCHER
Write-Output ("{0} total on-screen hitches: {1}" -f $Label, ($all | Measure-Object -Sum).Sum)
