#define _POSIX_C_SOURCE 200809L
#include "creme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#define BEUIP_PORT 9998
#define BEUIP_MAGIC "BEUIP"
#define BEUIP_MAGIC_LEN 5
#define LBUF 512
#define MAX_PEERS 255
#define BROADCAST_IP "192.168.88.255"
#define CREME_VERSION "1.1.0-tp3-etape1"

#ifdef TRACE
#define TRACEF(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACEF(...) do { } while (0)
#endif

typedef struct {
    struct in_addr addr;
    char pseudo[128];
    int used;
} participant_t;

static participant_t table_peers[MAX_PEERS];
static int sid = -1;
static char my_pseudo[128];
static struct in_addr my_ip;
static pthread_t udp_thread;
static int udp_thread_started = 0;
static volatile int running = 0;

static pthread_mutex_t peers_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static int is_my_ip(struct in_addr addr)
{
    return addr.s_addr == my_ip.s_addr;
}

static int find_participant_by_ip_nolock(struct in_addr addr)
{
    int i;
    for (i = 0; i < MAX_PEERS; ++i) {
        if (table_peers[i].used && table_peers[i].addr.s_addr == addr.s_addr) {
            return i;
        }
    }
    return -1;
}

static int find_participant_by_pseudo_nolock(const char *pseudo)
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

    pthread_mutex_lock(&peers_mutex);

    idx_ip = find_participant_by_ip_nolock(addr);
    if (idx_ip >= 0) {
        trim_copy(table_peers[idx_ip].pseudo, sizeof(table_peers[idx_ip].pseudo), pseudo);
        pthread_mutex_unlock(&peers_mutex);
        TRACEF("[TRACE] mise a jour %s -> %s\n", pseudo, addr_to_str(addr));
        return;
    }

    idx_pseudo = find_participant_by_pseudo_nolock(pseudo);
    if (idx_pseudo >= 0) {
        table_peers[idx_pseudo].addr = addr;
        pthread_mutex_unlock(&peers_mutex);
        TRACEF("[TRACE] pseudo connu, IP mise a jour %s -> %s\n", pseudo, addr_to_str(addr));
        return;
    }

    for (i = 0; i < MAX_PEERS; ++i) {
        if (!table_peers[i].used) {
            table_peers[i].used = 1;
            table_peers[i].addr = addr;
            trim_copy(table_peers[i].pseudo, sizeof(table_peers[i].pseudo), pseudo);
            pthread_mutex_unlock(&peers_mutex);
            printf("[TABLE] ajout : %s -> %s\n", pseudo, addr_to_str(addr));
            return;
        }
    }

    pthread_mutex_unlock(&peers_mutex);
    fprintf(stderr, "Table pleine : impossible d'ajouter %s (%s)\n", pseudo, addr_to_str(addr));
}

static void remove_participant_by_ip(struct in_addr addr)
{
    int idx;

    pthread_mutex_lock(&peers_mutex);
    idx = find_participant_by_ip_nolock(addr);
    if (idx >= 0) {
        printf("[TABLE] suppression : %s -> %s\n", table_peers[idx].pseudo, addr_to_str(addr));
        table_peers[idx].used = 0;
        table_peers[idx].pseudo[0] = '\0';
        table_peers[idx].addr.s_addr = 0;
    }
    pthread_mutex_unlock(&peers_mutex);
}

static void print_table(void)
{
    int i;
    int count = 0;

    pthread_mutex_lock(&peers_mutex);
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
    pthread_mutex_unlock(&peers_mutex);
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

static int send_packet_to(int sock, struct sockaddr_in *dst, char code, const char *payload)
{
    char packet[LBUF + 1];
    ssize_t packet_len = build_message(code, payload, packet, sizeof(packet));

    if (packet_len < 0) {
        perror("build_message");
        return -1;
    }

    if (sendto(sock, packet, (size_t)packet_len, 0,
               (struct sockaddr *)dst, sizeof(*dst)) == -1) {
        perror("sendto");
        return -1;
    }

    return 0;
}

static int send_broadcast(char code, const char *payload)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);
    dst.sin_addr.s_addr = inet_addr(BROADCAST_IP);
    return send_packet_to(sid, &dst, code, payload);
}

static int send_to_ip(struct in_addr addr, char code, const char *payload)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);
    dst.sin_addr = addr;
    return send_packet_to(sid, &dst, code, payload);
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

static int commande(char octet1, char *message, char *pseudo)
{
    int rc = 0;

    if (!running || sid < 0) {
        errno = ENOTCONN;
        return -1;
    }

    switch (octet1) {
        case '3':
            print_table();
            break;

        case '4': {
            int idx;
            struct in_addr dest;

            if (pseudo == NULL || *pseudo == '\0' || message == NULL) {
                errno = EINVAL;
                return -1;
            }

            pthread_mutex_lock(&peers_mutex);
            idx = find_participant_by_pseudo_nolock(pseudo);
            if (idx < 0) {
                pthread_mutex_unlock(&peers_mutex);
                errno = ENOENT;
                fprintf(stderr, "Pseudo inconnu : %s\n", pseudo);
                return -1;
            }
            dest = table_peers[idx].addr;
            pthread_mutex_unlock(&peers_mutex);

            rc = send_to_ip(dest, '9', message);
            if (rc == 0) {
                printf("[PRIVE] message envoye a %s (%s)\n", pseudo, addr_to_str(dest));
            }
            break;
        }

        case '5': {
            int i;
            struct in_addr dests[MAX_PEERS];
            char pseudos[MAX_PEERS][128];
            int count = 0;

            if (message == NULL) {
                errno = EINVAL;
                return -1;
            }

            pthread_mutex_lock(&peers_mutex);
            for (i = 0; i < MAX_PEERS; ++i) {
                if (!table_peers[i].used) {
                    continue;
                }
                if (is_my_ip(table_peers[i].addr)) {
                    continue;
                }
                dests[count] = table_peers[i].addr;
                trim_copy(pseudos[count], sizeof(pseudos[count]), table_peers[i].pseudo);
                ++count;
            }
            pthread_mutex_unlock(&peers_mutex);

            for (i = 0; i < count; ++i) {
                if (send_to_ip(dests[i], '9', message) == 0) {
                    printf("[DIFFUSION] message envoye a %s (%s)\n",
                           pseudos[i], addr_to_str(dests[i]));
                }
            }
            break;
        }

        default:
            errno = EINVAL;
            fprintf(stderr, "Commande locale non supportee : %c\n", octet1);
            return -1;
    }

    return rc;
}

static void *serveur_udp(void *p)
{
    char *pseudo = (char *)p;
    struct sockaddr_in local_addr;
    struct sockaddr_in from;
    socklen_t from_len;
    int opt = 1;
    char buf[LBUF + 1];
    ssize_t n;

    memset(table_peers, 0, sizeof(table_peers));
    trim_copy(my_pseudo, sizeof(my_pseudo), pseudo);

    if (get_primary_ipv4(&my_ip) == -1) {
        pthread_mutex_lock(&state_mutex);
        running = 0;
        udp_thread_started = 0;
        pthread_mutex_unlock(&state_mutex);
        free(pseudo);
        return NULL;
    }

    sid = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sid < 0) {
        perror("socket");
        pthread_mutex_lock(&state_mutex);
        running = 0;
        udp_thread_started = 0;
        pthread_mutex_unlock(&state_mutex);
        free(pseudo);
        return NULL;
    }

    if (setsockopt(sid, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        close(sid);
        sid = -1;
        pthread_mutex_lock(&state_mutex);
        running = 0;
        udp_thread_started = 0;
        pthread_mutex_unlock(&state_mutex);
        free(pseudo);
        return NULL;
    }

    if (setsockopt(sid, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) == -1) {
        perror("setsockopt SO_BROADCAST");
        close(sid);
        sid = -1;
        pthread_mutex_lock(&state_mutex);
        running = 0;
        udp_thread_started = 0;
        pthread_mutex_unlock(&state_mutex);
        free(pseudo);
        return NULL;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(BEUIP_PORT);

    if (bind(sid, (struct sockaddr *)&local_addr, sizeof(local_addr)) == -1) {
        perror("bind");
        close(sid);
        sid = -1;
        pthread_mutex_lock(&state_mutex);
        running = 0;
        udp_thread_started = 0;
        pthread_mutex_unlock(&state_mutex);
        free(pseudo);
        return NULL;
    }

    printf("Serveur BEUIP (thread UDP) lance avec le pseudo '%s' sur le port %d\n",
           my_pseudo, BEUIP_PORT);
    printf("IP locale detectee : %s\n", addr_to_str(my_ip));
    printf("Broadcast : %s\n", BROADCAST_IP);

    add_or_update_participant(my_ip, my_pseudo);

    if (send_broadcast('1', my_pseudo) == -1) {
        close(sid);
        sid = -1;
        pthread_mutex_lock(&state_mutex);
        running = 0;
        udp_thread_started = 0;
        pthread_mutex_unlock(&state_mutex);
        free(pseudo);
        return NULL;
    }
    printf("Annonce de presence envoyee en broadcast\n");

    while (running) {
        char code;
        char *payload;
        size_t payload_len;

        from_len = sizeof(from);
        n = recvfrom(sid, buf, LBUF, 0, (struct sockaddr *)&from, &from_len);
        if (n == -1) {
            if (!running) {
                break;
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
        TRACEF("[TRACE] recu code=%c depuis %s\n", code, addr_to_str(from.sin_addr));

        switch (code) {
            case '1':
                if (payload_len == 0) {
                    break;
                }
                add_or_update_participant(from.sin_addr, payload);
                if (!is_my_ip(from.sin_addr)) {
                    (void)send_to_ip(from.sin_addr, '2', my_pseudo);
                }
                break;

            case '2':
                if (payload_len == 0) {
                    break;
                }
                add_or_update_participant(from.sin_addr, payload);
                break;

            case '9': {
                int idx;
                char pseudo_src[128];
                pseudo_src[0] = '\0';

                pthread_mutex_lock(&peers_mutex);
                idx = find_participant_by_ip_nolock(from.sin_addr);
                if (idx >= 0) {
                    trim_copy(pseudo_src, sizeof(pseudo_src), table_peers[idx].pseudo);
                }
                pthread_mutex_unlock(&peers_mutex);

                if (idx >= 0) {
                    printf("Message de %s : %s\n", pseudo_src, payload);
                } else {
                    printf("Message de %s : %s (pseudo inconnu)\n", addr_to_str(from.sin_addr), payload);
                }
                break;
            }

            case '0':
                remove_participant_by_ip(from.sin_addr);
                break;

            case '3':
            case '4':
            case '5':
                fprintf(stderr,
                        "Tentative de commande interdite recue depuis le reseau (%c depuis %s)\n",
                        code, addr_to_str(from.sin_addr));
                break;

            default:
                fprintf(stderr, "Code inconnu : %c\n", code);
                break;
        }
    }

    if (sid >= 0) {
        close(sid);
        sid = -1;
    }

    pthread_mutex_lock(&state_mutex);
    udp_thread_started = 0;
    pthread_mutex_unlock(&state_mutex);

    free(pseudo);
    return NULL;
}

int creme_start_server(const char *pseudo)
{
    int rc;
    char *pseudo_copy;

    if (pseudo == NULL || *pseudo == '\0') {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&state_mutex);
    if (udp_thread_started) {
        pthread_mutex_unlock(&state_mutex);
        errno = EALREADY;
        return -1;
    }

    pseudo_copy = strdup(pseudo);
    if (pseudo_copy == NULL) {
        pthread_mutex_unlock(&state_mutex);
        errno = ENOMEM;
        return -1;
    }

    running = 1;
    rc = pthread_create(&udp_thread, NULL, serveur_udp, pseudo_copy);
    if (rc != 0) {
        running = 0;
        free(pseudo_copy);
        pthread_mutex_unlock(&state_mutex);
        errno = rc;
        return -1;
    }

    udp_thread_started = 1;
    pthread_mutex_unlock(&state_mutex);
    return 0;
}

int creme_stop_server(void)
{
    int local_sid;

    pthread_mutex_lock(&state_mutex);
    if (!udp_thread_started) {
        pthread_mutex_unlock(&state_mutex);
        errno = ESRCH;
        return -1;
    }

    if (sid >= 0) {
        (void)send_broadcast('0', my_pseudo);
    }

    running = 0;
    local_sid = sid;
    if (local_sid >= 0) {
        close(local_sid);
        sid = -1;
    }
    pthread_mutex_unlock(&state_mutex);

    if (pthread_join(udp_thread, NULL) != 0) {
        return -1;
    }

    pthread_mutex_lock(&peers_mutex);
    memset(table_peers, 0, sizeof(table_peers));
    pthread_mutex_unlock(&peers_mutex);

    return 0;
}

int creme_list_peers(void)
{
    return commande('3', NULL, NULL);
}

int creme_send_to_peer(const char *pseudo, const char *message)
{
    return commande('4', (char *)message, (char *)pseudo);
}

int creme_send_all(const char *message)
{
    return commande('5', (char *)message, NULL);
}

pid_t creme_server_pid(void)
{
    return udp_thread_started ? (pid_t)getpid() : (pid_t)-1;
}

const char *creme_version_string(void)
{
    return CREME_VERSION;
}
