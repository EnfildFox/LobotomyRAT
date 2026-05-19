// Modules/FileManager/filemgr.h
#pragma once
#include <windows.h>

// 🔑 ВАЖНО: Функции должны быть указателями (*)
struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api);

// Обработчики
void HandleDir(const char* path, ModuleAPI* api);
void HandleGet(const char* filepath, ModuleAPI* api);
void HandlePut(const char* filepath, const char* base64_data, ModuleAPI* api);
void HandleDel(const char* path, ModuleAPI* api);
void HandleRename(const char* old_path, const char* new_path, ModuleAPI* api);
void HandleMkdir(const char* path, ModuleAPI* api);
void HandleInfo(const char* path, ModuleAPI* api);

// Утилиты
char* Base64Encode(const unsigned char* data, size_t len, size_t* out_len);
unsigned char* Base64Decode(const char* data, size_t* out_len);
void SendJsonError(ModuleAPI* api, const char* cmd, const char* msg);
void SendJsonResult(ModuleAPI* api, const char* cmd, const char* data);