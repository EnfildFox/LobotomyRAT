// ModulesDemo/demo.cpp — MINIMAL WORKING VERSION
#include <windows.h>

// Экспорт функции Run (C-линковка, чтобы избежать name mangling)
extern "C" __declspec(dllexport) int __stdcall Run(void* api) {
    // Пустая функция — просто возвращаем успех
    (void)api; // Подавляем warning о неиспользуемом параметре
    return 0;
}

// Пустой DllMain — минимум кода, никаких сложных операций
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    // Запрещаем уведомления о потоках — это частая причина проблем при ручной загрузке
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}