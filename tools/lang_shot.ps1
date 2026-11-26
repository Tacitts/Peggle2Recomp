# Language check: start the game with a given language (the launcher's [gameplay] language) and
# save a screenshot from the menus, to see which languages the game itself shows.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\lang_shot.ps1 -Lang fr [-Frame 900] [-Seconds 40]
# The settings file is restored afterwards.
param([string]$Lang = 'en', [int]$Frame = 1500, [int]$Seconds = 60, [string]$AutoPress = 'A')

$root = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $root 'build\runtime'
$ini = Join-Path $runDir 'peggle2_settings.ini'
$backup = [IO.File]::ReadAllText($ini)
try {
    $text = $backup -replace '(?m)^language = .*$', "language = $Lang" -replace '(?m)^fps60 = .*$', 'fps60 = false'
    [IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding $false))
    $env:PEGGLE2_AUTOPRESS = $AutoPress
    $env:PEGGLE2_SCREENSHOT_FRAME = "$Frame"
    $before = Get-Date
    Set-Location $runDir
    $p = Start-Process -FilePath (Join-Path $runDir 'Peggle2.exe') -PassThru
    $null = $p.Handle
    if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); $p.WaitForExit() }
    Get-ChildItem (Join-Path $runDir 'screenshots') | Where-Object { $_.LastWriteTime -gt $before } | ForEach-Object FullName
    Select-String -Path (Join-Path $runDir 'peggle2_runtime.log') -Pattern 'xconfig\] .*setting 0x9|achievements\] .*language' | ForEach-Object Line
}
finally {
    [IO.File]::WriteAllText($ini, $backup, (New-Object Text.UTF8Encoding $false))
    Remove-Item Env:PEGGLE2_AUTOPRESS, Env:PEGGLE2_SCREENSHOT_FRAME -ErrorAction SilentlyContinue
}
