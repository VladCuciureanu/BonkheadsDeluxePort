@echo off
rem Build the port on Windows with the Visual Studio x64 toolchain.
rem   build.bat                      configure and build into build\
rem   build.bat -DBONKHEADS_SIT=...  extra arguments go to CMake
rem Set BUILD_DIR (default build) or BUILD_TYPE (default RelWithDebInfo) to change the
rem output folder or configuration.
setlocal
cd /d "%~dp0"
if not defined BUILD_DIR set "BUILD_DIR=build"
if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"

rem Use the compiler already on PATH (a "Native Tools" prompt), otherwise find
rem the newest Visual Studio with the C++ tools and load its environment.
where cl >nul 2>nul && goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs
echo Using Visual Studio in %VSDIR%
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs

where cmake >nul 2>nul || (echo error: CMake not found. Install the Visual Studio component "C++ CMake tools for Windows". & exit /b 1)
where ninja >nul 2>nul || (echo error: Ninja not found. Install the Visual Studio component "C++ CMake tools for Windows". & exit /b 1)

cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %* || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1
echo.
echo Build complete. Start the game with:  %BUILD_DIR%\bonkheads.exe
exit /b 0

:no_vs
echo error: no Visual Studio C++ compiler found.
echo Install Visual Studio 2022 or newer (the free Community edition is fine) with the
echo "Desktop development with C++" workload, then run build.bat again.
exit /b 1
