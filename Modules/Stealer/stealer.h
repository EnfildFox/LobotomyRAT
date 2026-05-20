// Modules/Stealer/stealer.h
#pragma once
#include <windows.h>

struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api);