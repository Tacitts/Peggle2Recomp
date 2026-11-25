# Converts every captured shader container (build/runtime/shaders/) to HLSL with XenosRecomp
# and compiles each one with DXC the way XenosRecomp's cache build does (vs_6_0 / ps_6_0, or
# lib_6_3 for shaders that use specialization constants). Writes <name>.hlsl and <name>.log
# next to build/shaders/ and prints a pass/fail summary.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\shaders.ps1 [-Filter <name>]
param([string]$Filter = '*', [string]$InDir = '', [string]$OutDir = '')

$root    = Split-Path -Parent $PSScriptRoot
$xenos   = Join-Path $root 'third_party\XenosRecomp\build\XenosRecomp\XenosRecomp.exe'
$dxc     = Join-Path $root 'third_party\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxc.exe'
$include = Join-Path $root 'third_party\XenosRecomp\XenosRecomp\shader_common.h'
$inDir   = if ($InDir) { $InDir } else { Join-Path $root 'build\runtime\shaders' }
$outDir  = if ($OutDir) { $OutDir } else { Join-Path $root 'build\shaders' }
New-Item -ItemType Directory -Force $outDir | Out-Null

$ok = 0; $fail = @()
$files = Get-ChildItem -File $inDir -Filter $Filter | Sort-Object Name
foreach ($f in $files) {
    $base = [IO.Path]::GetFileNameWithoutExtension($f.Name)
    $hlsl = Join-Path $outDir "$base.hlsl"
    $log  = Join-Path $outDir "$base.log"
    & $xenos $f.FullName $hlsl $include | Out-Null
    if (-not (Test-Path $hlsl)) { $fail += "$base (XenosRecomp produced nothing)"; continue }

    $src = Get-Content -Raw $hlsl
    $isPixel = $src -match 'PixelShaderConstants'
    if ($src -match 'g_SpecConstants\(\)') { $target = 'lib_6_3' }
    elseif ($isPixel) { $target = 'ps_6_0' } else { $target = 'vs_6_0' }

    # Through cmd so DXC's stderr is captured as plain text (PowerShell 5.1 wraps native stderr).
    $dxil = Join-Path $outDir "$base.dxil"
    $out = cmd /c "`"$dxc`" -T $target -HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug -Fo `"$dxil`" `"$hlsl`" 2>&1" | Out-String
    $code = $LASTEXITCODE
    $out | Set-Content -Encoding utf8 $log
    if ($code -eq 0) { $ok++ } else {
        $first = ($out -split "`n" | Where-Object { $_ -match 'error:' } | Select-Object -First 1) -replace '^.*?\.hlsl:', ''
        $fail += "$base [$target]: $($first.Trim())"
    }
}
"DXC: $ok/$($files.Count) compile"
$fail | ForEach-Object { "  FAIL $_" }
