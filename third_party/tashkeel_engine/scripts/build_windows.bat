@echo off
setlocal
cd /d "%~dp0\.."

where cl >nul 2>&1
if errorlevel 1 (
    for %%V in (
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if exist %%V (
            call %%V >nul
            goto :cmake_build
        )
    )
)

:cmake_build
cmake -B build_win -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build_win --config Release
if errorlevel 1 exit /b 1
if not exist windows mkdir windows
copy /Y build_win\Release\barq.dll windows\barq.dll >nul
copy /Y build_win\Release\barq.lib windows\barq.lib >nul
copy /Y build_win\Release\barq.exe windows\barq.exe >nul
echo Built Windows: windows\barq.dll, windows\barq.lib, windows\barq.exe
exit /b 0
