@echo off
setlocal
cd /d "%~dp0\.."

REM Option 1: MSVC / Visual Studio Build Tools via CMake
cmake -B build_win -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :mingw
cmake --build build_win --config Release
if errorlevel 1 goto :mingw
if not exist windows mkdir windows
copy /Y build_win\Release\barq.dll windows\barq.dll >nul
copy /Y build_win\Release\barq.lib windows\barq.lib >nul
copy /Y build_win\Release\barq.exe windows\barq.exe >nul
echo Built Windows: windows\barq.dll, windows\barq.lib, windows\barq.exe
exit /b 0

:mingw
echo Trying MinGW g++...
if not exist windows mkdir windows
g++ -std=c++17 -O3 -DBARQ_EXPORTS -DTASHKEEL_EXPORTS -Icpp/include -shared -o windows\barq.dll cpp\src\diacritizer.cpp cpp\src\c_api.cpp
g++ -std=c++17 -O3 -Icpp/include -o windows\barq.exe cpp\src\diacritizer.cpp cpp\src\cli.cpp
echo Built Windows: windows\barq.dll and windows\barq.exe
