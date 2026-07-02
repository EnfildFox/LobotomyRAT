@echo off
:: config_vs.bat - find Visual Studio vcvars64.bat
set "VS_VCVARS="
where vswhere >nul 2>nul
if errorlevel 1 (
    echo [!] vswhere not found. Please install Visual Studio Build Tools or VS 2022/2019.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`vswhere -latest -requires Microsoft.Component.MSBuild -find VC\Auxiliary\Build\vcvars64.bat`) do (
    set "VS_VCVARS=%%i"
    goto :found
)
echo [!] Visual Studio C++ tools not found. Install 'Desktop development with C++' workload.
exit /b 1
:found
echo [+] Found vcvars64.bat: %VS_VCVARS%
exit /b 0