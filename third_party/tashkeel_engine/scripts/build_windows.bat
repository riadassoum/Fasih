@echo off
setlocal
cd /d "%~dp0\.."

REM Option 1: MSVC / Visual Studio Build Tools via CMake
cmake -B build_win -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :mingw
cmake --build build_win --config Release
if errorlevel 1 goto :mingw
echo Built Windows DLL in build_win\Release\tashkeel.dll
exit /b 0

:mingw
echo Trying MinGW g++...
if not exist build mkdir build
g++ -std=c++17 -O3 -Icpp/include -shared -o build\tashkeel.dll cpp\src\diacritizer.cpp cpp\src\c_api.cpp
g++ -std=c++17 -O3 -Icpp/include -o build\tashkeel_cli.exe cpp\src\diacritizer.cpp cpp\src\cli.cpp
echo Built Windows: build\tashkeel.dll and build\tashkeel_cli.exe
