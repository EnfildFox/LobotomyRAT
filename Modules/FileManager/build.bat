@echo off
echo [0/3] Initializing MSVC environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [!] MSVC not found & pause & exit /b 1 )
echo [OK] Environment ready.

echo [1/3] Compiling filemgr.dll...
cl /LD /MT /O2 filemgr.cpp /Fe:filemgr.dll kernel32.lib user32.lib
if errorlevel 1 (
    echo [!] Compilation failed.
    pause
    exit /b 1
)

echo [2/3] Preparing target folder...
REM Используем относительный путь, чтобы избежать проблем с кириллицей в абсолютных путях
set REL_PATH=..\..\Server\modules
if not exist "%REL_PATH%" (
    mkdir "%REL_PATH%"
    echo [+] Created folder: %REL_PATH%
)

echo [3/3] Encrypting to filemgr.bin...
REM PowerShell выполнит шифрование и сохранит файл по относительному пути
powershell -Command "$d=[IO.File]::ReadAllBytes('filemgr.dll'); for($i=0;$i -lt $d.Length;$i++){$d[$i]=$d[$i]-bxor 0xAA}; [IO.File]::WriteAllBytes('%REL_PATH%\filemgr.bin',$d)"

if errorlevel 1 (
    echo [!] Encryption failed.
    pause
    exit /b 1
)

echo [+] Done: filemgr.bin is ready at %REL_PATH%
pause