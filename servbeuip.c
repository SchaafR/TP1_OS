/*****
* Etape 2 : serveur BEUIP
* - annonce sa presence en broadcast
* - maintient une table (IP + pseudo)
* - accepte des commandes locales sur 127.0.0.1:9998
* - relaie des messages sur le reseau
*****/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#define BEUIP_PORT 9998
#define BEUIP_MAGIC "BEUIP"
#define BEUIP_MAGIC_LEN 5
#define LBUF 512
#define MAX_PEERS 255
#define BROADCAST_IP "192.168.88.255"
#define LOCALHOST_IP "127.0.0.1"

typedef struct {
    struct in_addr addr;
    char pseudo[128];
    int used;
} participant_t;

static participant_t table_peers[MAX_PEERS];
static int sid = -1;
static char my_pseudo[128];
static struct in_addr my_ip;
static volatile sig_atomic_t running = 1;

static const char *addr_to_str(struct in_addr addr)
{
    return inet_ntoa(addr);
}

static void trim_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int is_localhost(struct in_addr addr)
{
    return ntohl(addr.s_addr) == INADDR_LOOPBACK;
}

static int is_my_ip(struct in_addr addr)
{
    return addr.s_addr == my_ip.s_addr;
}

static int find_participant_by_ip(struct in_addr addr)
{
    int i;
    for (i = 0; i < MAX_PEERS; ++i) {
        if (table_peers[i].used && table_peers[i].addr.s_addr == addr.s_addr) {
            return i;
        }
    }
    return -1;
}

static int find_participant_by_pseudo(const char *pseudo)
{
    int i;
    for (i = 0; i < MAX_PEERS; ++i) {
        if (table_peers[i].used && strcmp(table_peers[i].pseudo, pseudo) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_or_update_participant(struct in_addr addr, const char *pseudo)
{
    int i;
    int idx_ip;
    int idx_pseudo;

    if (pseudo == NULL || *pseudo == '\0') {
        return;
    }

    idx_ip = find_participant_by_ip(addr);
    if (idx_ip >= 0) {
        trim_copy(table_peers[idx_ip].pseudo, sizeof(table_peers[idx_ip].pseudo), pseudo);
        return;
    }

    idx_pseudo = find_participant_by_pseudo(pseudo);
    if (idx_pseudo >= 0) {
        table_peers[idx_pseudo].addr = addr;
        return;
    }

    for (i = 0; i < MAX_PEERS; ++i) {
        if (!table_peers[i].used) {
            table_peers[i].used = 1;
            table_peers[i].addr = addr;
            trim_copy(table_peers[i].pseudo, sizeof(table_peers[i].pseudo), pseudo);
            printf("[TABLE] ajout : %s -> %s\n", pseudo, addr_to_str(addr));
            return;
        }
    }

    fprintf(stderr, "Table pleine : impossible d'ajouter %s (%s)\n", pseudo, addr_to_str(addr));
}

static void remove_participant_by_ip(struct in_addr addr)
{
    int idx = find_participant_by_ip(addr);
    if (idx >= 0) {
        printf("[TABLE] suppression : %s -> %s\n", table_peers[idx].pseudo, addr_to_str(addr));
        table_peers[idx].used = 0;
        table_peers[idx].pseudo[0] = '\0';
        table_peers[idx].addr.s_addr = 0;
    }
}

static void print_table(void)
{
    int i;
    int count = 0;

    printf("\n=== Liste des presents ===\n");
    for (i = 0; i < MAX_PEERS; ++i) {
        if (table_peers[i].used) {
            printf("- %-20s %s\n", table_peers[i].pseudo, addr_to_str(table_peers[i].addr));
            ++count;
        }
    }
    if (count == 0) {
        printf("(aucun participant connu)\n");
    }
    printf("==========================\n\n");
}

static ssize_t build_message(char code, const char *payload, char *out, size_t out_size)
{
    size_t payload_len = payload ? strlen(payload) : 0;
    size_t needed = 1 + BEUIP_MAGIC_LEN + payload_len;

    if (needed > out_size) {
        errno = EMSGSIZE;
        return -1;
    }

    out[0] = code;
    memcpy(out + 1, BEUIP_MAGIC, BEUIP_MAGIC_LEN);
    if (payload_len > 0) {
        memcpy(out + 1 + BEUIP_MAGIC_LEN, payload, payload_len);
    }
    return (ssize_t)needed;
}

static int send_packet_to(struct sockaddr_in *dst, char code, const char *payload)
{
    char packet[LBUF + 1];
    ssize_t packet_len = build_message(code, payload, packet, sizeof(packet));

    if (packet_len < 0) {
        perror("build_message");
        return -1;
    }

    if (sendto(sid, packet, (size_t)packet_len, 0,
               (struct sockaddr *)dst, sizeof(*dst)) == -1) {
        perror("sendto");
        return -1;
    }

    return 0;
}

static int send_broadcast(char code, const char *payload)
{
    struct sockaddr_in dst;
    bzero(&dst, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);
    dst.sin_addr.s_addr = inet_addr(BROADCAST_IP);
    return send_packet_to(&dst, code, payload);
}

static int send_to_ip(struct in_addr addr, char code, const char *payload)
{
    struct sockaddr_in dst;
    bzero(&dst, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);
    dst.sin_addr = addr;
    return send_packet_to(&dst, code, payload);
}

static int get_primary_ipv4(struct in_addr *result)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        struct sockaddr_in *sa;
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (ntohl(sa->sin_addr.s_addr) == INADDR_LOOPBACK) {
            continue;
        }
        *result = sa->sin_addr;
        freeifaddrs(ifaddr);
        return 0;
    }

    freeifaddrs(ifaddr);
    result->s_addr = htonl(INADDR_LOOPBACK);
    return 0;
}

static void on_signal(int signo)
{
    (void)signo;
    running = 0;
}

static void send_leave_and_close(void)
{
    if (sid >= 0) {
        send_broadcast('0', my_pseudo);
        close(sid);
        sid = -1;
    }
}

int main(int argc, char *argv[])
{
    struct sockaddr_in local_addr;
    struct sockaddr_in from;
    socklen_t from_len;
    int opt = 1;
    char buf[LBUF + 1];
    ssize_t n;

    if (argc != 2) {
        fprintf(stderr, "Utilisation : %s pseudo\n", argv[0]);
        return 1;
    }

    trim_copy(my_pseudo, sizeof(my_pseudo), argv[1]);

    if (get_primary_ipv4(&my_ip) == -1) {
        return 2;
    }

    sid = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sid < 0) {
        perror("socket");
        return 3;
    }

    if (setsockopt(sid, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        close(sid);
        return 4;
    }

    if (setsockopt(sid, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) == -1) {
        perror("setsockopt SO_BROADCAST");
        close(sid);
        return 5;
    }

    bzero(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(BEUIP_PORT);

    if (bind(sid, (struct sockaddr *)&local_addr, sizeof(local_addr)) == -1) {
        perror("bind");
        close(sid);
        return 6;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("Serveur BEUIP lance avec le pseudo '%s' sur le port %d\n", my_pseudo, BEUIP_PORT);
    printf("IP locale detectee : %s\n", addr_to_str(my_ip));
    printf("Broadcast : %s\n", BROADCAST_IP);

    add_or_update_participant(my_ip, my_pseudo);

    if (send_broadcast('1', my_pseudo) == -1) {
        send_leave_and_close();
        return 7;
    }
    printf("Annonce de presence envoyee en broadcast\n");

    while (running) {
        char code;
        char *payload;
        size_t payload_len;

        from_len = sizeof(from);
        n = recvfrom(sid, buf, LBUF, 0, (struct sockaddr *)&from, &from_len);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            break;
        }

        if (n < (ssize_t)(1 + BEUIP_MAGIC_LEN)) {
            fprintf(stderr, "Message trop court ignore\n");
            continue;
        }

        buf[n] = '\0';
        code = buf[0];
        if (memcmp(buf + 1, BEUIP_MAGIC, BEUIP_MAGIC_LEN) != 0) {
            fprintf(stderr, "Message ignore : signature invalide\n");
            continue;
        }

        payload = buf + 1 + BEUIP_MAGIC_LEN;
        payload_len = (size_t)n - (1 + BEUIP_MAGIC_LEN);

        printf("[RECU] code=%c depuis %s\n", code, addr_to_str(from.sin_addr));

        switch (code) {
            case '1':
                if (payload_len == 0) {
                    break;
                }
                add_or_update_participant(from.sin_addr, payload);
                if (!is_my_ip(from.sin_addr)) {
                    if (send_to_ip(from.sin_addr, '2', my_pseudo) == 0) {
                        printf("[AR] accuse envoye a %s\n", addr_to_str(from.sin_addr));
                    }
                }
                break;

            case '2':
                if (payload_len == 0) {
                    break;
                }
                add_or_update_participant(from.sin_addr, payload);
                break;

            case '3':
                if (!is_localhost(from.sin_addr)) {
                    fprintf(stderr, "Commande liste refusee : source non locale (%s)\n", addr_to_str(from.sin_addr));
                    break;
                }
                print_table();
                break;

            case '4': {
                char *dest_pseudo;
                char *message;
                int idx;

                if (!is_localhost(from.sin_addr)) {
                    fprintf(stderr, "Commande message prive refusee : source non locale (%s)\n", addr_to_str(from.sin_addr));
                    break;
                }

                dest_pseudo = payload;
                message = memchr(payload, '\0', payload_len);
                if (message == NULL || (size_t)(message - payload) >= payload_len) {
                    fprintf(stderr, "Format code 4 invalide\n");
                    break;
                }
                ++message;
                idx = find_participant_by_pseudo(dest_pseudo);
                if (idx < 0) {
                    fprintf(stderr, "Pseudo inconnu : %s\n", dest_pseudo);
                    break;
                }
                if (send_to_ip(table_peers[idx].addr, '9', message) == 0) {
                    printf("[PRIVE] message envoye a %s (%s)\n",
                           table_peers[idx].pseudo,
                           addr_to_str(table_peers[idx].addr));
                }
                break;
            }

            case '5': {
                int i;
                if (!is_localhost(from.sin_addr)) {
                    fprintf(stderr, "Commande diffusion refusee : source non locale (%s)\n", addr_to_str(from.sin_addr));
                    break;
                }
                for (i = 0; i < MAX_PEERS; ++i) {
                    if (!table_peers[i].used) {
                        continue;
                    }
                    if (is_my_ip(table_peers[i].addr)) {
                        continue;
                    }
                    if (send_to_ip(table_peers[i].addr, '9', payload) == 0) {
                        printf("[DIFFUSION] message envoye a %s (%s)\n",
                               table_peers[i].pseudo,
                               addr_to_str(table_peers[i].addr));
                    }
                }
                break;
            }

            case '9': {
                int idx = find_participant_by_ip(from.sin_addr);
                if (idx >= 0) {
                    printf("Message de %s : %s\n", table_peers[idx].pseudo, payload);
                } else {
                    printf("Message de %s : %s (pseudo inconnu)\n", addr_to_str(from.sin_addr), payload);
                }
                break;
            }

            case '0':
                remove_participant_by_ip(from.sin_addr);
                break;

            default:
                fprintf(stderr, "Code inconnu : %c\n", code);
                break;
        }
    }

    printf("Arret du serveur, envoi du message de depart...\n");
    send_leave_and_close();
    return 0;
}
