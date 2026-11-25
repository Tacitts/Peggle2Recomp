@echo off
rem Build helper: sets up MSVC x64 + LLVM clang-cl, then builds the given CMake source dir.
rem Usage: build.cmd <source_dir> <build_dir> [extra cmake args...]
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
set SRC=%~1
set BLD=%~2
shift
shift
cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl %1 %2 %3 %4 || exit /b 1
rem Optional: set BUILD_ARGS (e.g. "-- -k 0" to keep going after failures).
cmake --build "%BLD%" %BUILD_ARGS% || exit /b 1
