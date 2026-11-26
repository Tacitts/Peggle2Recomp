# Loading benchmark: boot, walk into a level (PEGGLE2_AUTOPRESS=A) and total how late frames
# were, from the runtime's "[frames]" lines (logged every 10 s).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench_load.ps1 [-Runs 3] [-Seconds 45] [-Label name]
#   game late ms   = time past 34.5 ms, summed over every late game frame (VdSwap to VdSwap)
#   shown late ms  = the same for frames reaching the screen (presents of a new frame)
param([int]$Runs = 3, [int]$Seconds = 45, [string]$Label = '')

$root = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $root 'build\runtime'
$env:PEGGLE2_AUTOPRESS = 'A'
Remove-Item Env:PEGGLE2_PROFILE -ErrorAction SilentlyContinue
$rows = @()
for ($i = 1; $i -le $Runs; $i++) {
    Set-Location $runDir
    $p = Start-Process -FilePath (Join-Path $runDir 'Peggle2.exe') -ArgumentList "`"$root\game`"" -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'bench_stdout.txt') -RedirectStandardError (Join-Path $runDir 'bench_stderr.txt')
    $null = $p.Handle
    $alive = -not $p.WaitForExit($Seconds * 1000)
    if ($alive) { $p.Kill(); $p.WaitForExit() }
    Start-Sleep -Milliseconds 300
    $lines = Get-Content (Join-Path $runDir 'peggle2_runtime.log')
    $r = [ordered]@{ Run = $i; GameLate = 0.0; ShownLate = 0.0; Over50 = 0; Over100 = 0; MaxMs = 0.0 }
    foreach ($l in $lines) {
        if ($l -match '\[frames\] game swaps: \d+, avg [\d.]+ ms, max ([\d.]+) ms, late by ([\d.]+) ms.*<=67 (\d+) <=100 (\d+) <=250 (\d+) >250 (\d+)') {
            $r.MaxMs = [Math]::Max($r.MaxMs, [double]$Matches[1])
            $r.GameLate += [double]$Matches[2]
            $r.Over50 += [int]$Matches[3] + [int]$Matches[4] + [int]$Matches[5] + [int]$Matches[6]
            $r.Over100 += [int]$Matches[5] + [int]$Matches[6]
        }
        elseif ($l -match '\[frames\] new frames shown: \d+, avg [\d.]+ ms, max [\d.]+ ms, late by ([\d.]+) ms') {
            $r.ShownLate += [double]$Matches[1]
        }
    }
    $crash = ($lines | Select-String '==== CRASH|==== indirect').Count
    $row = [pscustomobject]$r
    $rows += $row
    Write-Output ("{0} run {1}: game frames late by {2:N0} ms in total ({3} over 50 ms, {4} over 100 ms, worst {5:N1} ms); shown frames late by {6:N0} ms{7}" -f `
        $Label, $i, $row.GameLate, $row.Over50, $row.Over100, $row.MaxMs, $row.ShownLate, $(if ($crash) { ', CRASHED' } elseif (-not $alive) { ', EXITED' } else { '' }))
}
$g = $rows | Measure-Object GameLate -Average -Minimum -Maximum
$s = $rows | Measure-Object ShownLate -Average
Write-Output ("{0} mean: game frames late by {1:N0} ms (min {2:N0}, max {3:N0}), shown frames late by {4:N0} ms, {5:N1} frames over 100 ms per run ({6} runs)" -f `
    $Label, $g.Average, $g.Minimum, $g.Maximum, $s.Average, ($rows | Measure-Object Over100 -Average).Average, $Runs)
