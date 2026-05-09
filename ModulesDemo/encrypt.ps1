# ModulesDemo/encrypt.ps1 — СКРИПТ ШИФРОВАНИЯ МОДУЛЯ (ЗАПУСКАТЬ ИЗ ModulesDemo/)
# Сохрани этот файл как ModulesDemo/encrypt.ps1 и запускай: .\encrypt.ps1

# Убеждаемся, что мы в правильной папке
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $scriptDir

Write-Host "[*] Encrypting demo.dll..." -ForegroundColor Cyan

# Читаем DLL
$dllPath = Join-Path $scriptDir "demo.dll"
if (-not (Test-Path $dllPath)) {
    Write-Host "[!] demo.dll not found. Compile it first:" -ForegroundColor Red
    Write-Host "    cl /LD /DYNAMICBASE /NXCOMPAT demo.cpp /Fe:demo.dll" -ForegroundColor Yellow
    pause; exit 1
}
$dll = [System.IO.File]::ReadAllBytes($dllPath)

# XOR 0xAA
for($i=0; $i -lt $dll.Length; $i++) { $dll[$i] = $dll[$i] -bxor 0xAA }

# Сохраняем в Server/modules/
$projectRoot = Split-Path -Parent $scriptDir
$modulesDir = Join-Path $projectRoot "Server\modules"
if (-not (Test-Path $modulesDir)) {
    New-Item -ItemType Directory -Force -Path $modulesDir | Out-Null
    Write-Host "[+] Created modules directory: $modulesDir"
}
$destPath = Join-Path $modulesDir "demo.bin"
[System.IO.File]::WriteAllBytes($destPath, $dll)

Write-Host "[+] Module encrypted and saved to: $destPath" -ForegroundColor Green
Write-Host "[*] Ready for LOAD_MODULE demo"
pause