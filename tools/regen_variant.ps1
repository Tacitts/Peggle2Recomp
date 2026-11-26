# Regenerate the guest code with a set of XenonRecomp register options and rebuild the runtime.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\regen_variant.ps1 -Name A -Options ctr_as_local,xer_as_local
#   -Options  the *_as_local / skip_* options to turn on (all others off)
#   -Name     the output is kept in ppc_<Name>\ so variants can be swapped without regenerating
# The current ppc\ is moved to ppc_<PreviousName>\ (from ppc\.variant, default "off") first.
param([string]$Name, [string[]]$Options = @())

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$all = 'skip_lr', 'skip_msr', 'ctr_as_local', 'xer_as_local', 'reserved_as_local', 'cr_as_local', 'non_argument_as_local', 'non_volatile_as_local'
$Options = @($Options | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes "a,b" as one string
foreach ($o in $Options) { if ($all -notcontains $o) { throw "unknown option $o" } }

# Park the current output.
$prev = if (Test-Path ppc\.variant) { (Get-Content ppc\.variant -Raw).Trim() } else { 'off' }
if (Test-Path ppc) {
    if (Test-Path "ppc_$prev") { Remove-Item -Recurse -Force "ppc_$prev" }
    Rename-Item ppc "ppc_$prev"
}

if (Test-Path "ppc_$Name") {
    Write-Output "reusing ppc_$Name"
    Rename-Item "ppc_$Name" ppc
    # Mark the files as changed: their old timestamps would let Ninja keep the other variant's
    # objects, mixing two PPCContext layouts (that crashed with unmasked FP exceptions).
    $now = Get-Date
    Get-ChildItem ppc -File | ForEach-Object { $_.LastWriteTime = $now }
} else {
    # Options in the base config, then regenerate into an empty ppc\.
    $cfg = Get-Content config\peggle2.base.toml
    $cfg = $cfg | ForEach-Object {
        $line = $_
        foreach ($o in $all) {
            if ($line -match "^$o\s*=") { $line = "$o = " + $(if ($Options -contains $o) { 'true' } else { 'false' }) }
        }
        $line
    }
    # UTF-8 without a BOM and LF line endings: XenonRecomp aborts (0xC0000409) on the BOM that
    # Windows PowerShell's Set-Content -Encoding utf8 writes.
    [IO.File]::WriteAllText((Join-Path $root 'config\peggle2.base.toml'), (($cfg -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
    New-Item -ItemType Directory ppc | Out-Null
    python -I tools\gen_config.py . | Out-Null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & .\third_party\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\peggle2.toml third_party\XenonRecomp\XenonUtils\ppc_context.h > "build\recomp_$Name.log"
    Write-Output ("recompiled in {0:N0} s: {1} files" -f $sw.Elapsed.TotalSeconds, (Get-ChildItem ppc\*.cpp).Count)
}
Set-Content -NoNewline -Encoding ascii ppc\.variant $Name

$sw = [Diagnostics.Stopwatch]::StartNew()
cmd /c 'tools\build.cmd runtime build\runtime "-DPEGGLE2_FFMPEG_ROOT=C:/dev/vcpkg/installed/x64-windows-static"' | Select-String 'error|FAILED|Linking' | Select-Object -First 10
Write-Output ("built in {0:N0} s (variant {1}: {2})" -f $sw.Elapsed.TotalSeconds, $Name, ($Options -join ', '))
