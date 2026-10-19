@echo off
rem Configure and build with the Visual Studio toolchain (x64).
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo %* || exit /b 1
cmake --build build || exit /b 1
