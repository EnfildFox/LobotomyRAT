//пока не работает
#include <windows.h>
#include <stdio.h>

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

//точка входа модуля, вызываемая агентом после рефлективной загрузки
extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api) return -1;
    api->log("[SCREENSHOT] Module loaded, Run called\n");
    api->send_result((const unsigned char*)"SCREENSHOT_MODULE_LOADED\n", 25);
    return 0;
}

// вызывается при загрузке ивыгрузке длл
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}