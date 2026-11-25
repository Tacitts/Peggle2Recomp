# Dot-source this to get git/python/cmake/ninja/clang plus the MSVC x64 environment.
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') + ';C:\Program Files\LLVM\bin'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
# Keep LLVM first so clang-cl is the 23.x install, not an older bundled one.
$env:Path = 'C:\Program Files\LLVM\bin;' + $env:Path
$global:PY = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
