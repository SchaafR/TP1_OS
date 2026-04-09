#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
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
#include <netdb.h>
#include <net/if.h>

#ifdef TRACE
#define TRACE_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE_PRINT(...) ((void)0)
#endif

#define BEUIP_PORT 9998
#define BEUIP_MAGIC "BEUIP"
#define BEUIP_MAGIC_LEN 5
#define LBUF 512
#define LPSEUDO 23
#define MAX_BCAST 32
#define CREME_VERSION "1.2.0-tp3-etape2"

#ifdef TRACE
#define TRACEF(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACEF(...) do { } while (0)
#endif

struct elt {
    char nom[LPSEUDO + 1];
    char adip[16];
    struct elt *next;
};

typedef struct {
    struct in_addr addr;
} bcast_addr_t;

static int sid = -1;
static char my_pseudo[LPSEUDO + 1];
static pthread_t udp_thread;
static int udp_thread_started = 0;
static volatile int running = 0;

static struct elt *liste = NULL;
static pthread_mutex_t peers_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static bcast_addr_t bcasts[MAX_BCAST];
static int nb_bcasts = 0;
static struct in_addr my_ips[MAX_BCAST];
static int nb_my_ips = 0;

static void trim_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static const char *addr_to_str(struct in_addr addr)
{
    return inet_ntoa(addr);
}

static int is_loopback_addr(struct in_addr addr)
{
    return ((ntohl(addr.s_addr) & 0xFF000000u) == 0x7F000000u);
}

static int is_my_ip(struct in_addr addr)
{
    int i;
    for (i = 0; i < nb_my_ips; ++i) {
        if (my_ips[i].s_addr == addr.s_addr) {
            return 1;
        }
    }
    return 0;
}

static void videListe_nolock(void)
{
    struct elt *cur = liste;
    while (cur != NULL) {
        struct elt *next = cur->next;
        free(cur);
        cur = next;
    }
    liste = NULL;
}

static struct elt *find_by_ip_nolock(const char *adip)
{
    struct elt *cur;
    for (cur = liste; cur != NULL; cur = cur->next) {
        if (strcmp(cur->adip, adip) == 0) {
            return cur;
        }
    }
    return NULL;
}

static struct elt *find_by_pseudo_nolock(const char *pseudo)
{
    struct elt *cur;
    for (cur = liste; cur != NULL; cur = cur->next) {
        if (strcmp(cur->nom, pseudo) == 0) {
            return cur;
        }
    }
    return NULL;
}

static int ip_string_to_addr(const char *ip, struct in_addr *addr)
{
    if (inet_pton(AF_INET, ip, addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int collect_interfaces(void)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;

    nb_bcasts = 0;
    nb_my_ips = 0;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        char host[64];
        struct sockaddr_in *sa;
        struct in_addr ip;
        int already;
        int i;

        if (ifa->ifa_addr == NULL) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_BROADCAST)) {
            continue;
        }
        if (ifa->ifa_broadaddr == NULL) {
            continue;
        }

        sa = (struct sockaddr_in *)ifa->ifa_addr;
        ip = sa->sin_addr;
        if (!is_loopback_addr(ip) && nb_my_ips < MAX_BCAST) {
            already = 0;
            for (i = 0; i < nb_my_ips; ++i) {
                if (my_ips[i].s_addr == ip.s_addr) {
                    already = 1;
                    break;
                }
            }
            if (!already) {
                my_ips[nb_my_ips++] = ip;
                TRACEF("[TRACE] iface %s ip=%s\n", ifa->ifa_name, addr_to_str(ip));
            }
        }

        if (getnameinfo(ifa->ifa_broadaddr,
                        sizeof(struct sockaddr_in),
                        host, sizeof(host),
                        NULL, 0, NI_NUMERICHOST) != 0) {
            continue;
        }

        if (strcmp(host, "127.0.0.1") == 0) {
            continue;
        }

        if (nb_bcasts < MAX_BCAST) {
            struct in_addr baddr;
            if (inet_aton(host, &baddr) != 0) {
                already = 0;
                for (i = 0; i < nb_bcasts; ++i) {
                    if (bcasts[i].addr.s_addr == baddr.s_addr) {
                        already = 1;
                        break;
                    }
                }
                if (!already) {
                    bcasts[nb_bcasts++].addr = baddr;
                    TRACEF("[TRACE] iface %s broadcast=%s\n", ifa->ifa_name, host);
                }
            }
        }
    }

    freeifaddrs(ifaddr);

    if (nb_my_ips == 0) {
        my_ips[0].s_addr = htonl(INADDR_LOOPBACK);
        nb_my_ips = 1;
    }

    return 0;
}

static int send_packet_to(int sock, struct sockaddr_in *dst, char code, const char *payload)
{
    char packet[LBUF + 1];
    size_t payload_len = payload ? strlen(payload) : 0;
    size_t packet_len = 1 + BEUIP_MAGIC_LEN + payload_len;

    if (packet_len > sizeof(packet)) {
        errno = EMSGSIZE;
        return -1;
    }

    packet[0] = code;
    memcpy(packet + 1, BEUIP_MAGIC, BEUIP_MAGIC_LEN);
    if (payload_len > 0) {
        memcpy(packet + 1 + BEUIP_MAGIC_LEN, payload, payload_len);
    }

    if (sendto(sock, packet, packet_len, 0, (struct sockaddr *)dst, sizeof(*dst)) == -1) {
        return -1;
    }
    return 0;
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

static void send_to_all_broadcasts(char code, const char *payload)
{
    int i;
    for (i = 0; i < nb_bcasts; ++i) {
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(BEUIP_PORT);
        dst.sin_addr = bcasts[i].addr;
        if (send_packet_to(sid, &dst, code, payload) == -1) {
            perror("sendto broadcast");
        }
    }
}

static void ajouteElt(char *pseudo, char *adip)
{
    TRACE_PRINT("[TRACE] ajout pseudo=%s ip=%s\n", pseudo, adip);
    struct elt *cur;
    struct elt *prev;
    struct elt *node;

    if (pseudo == NULL || adip == NULL || *pseudo == '\0' || *adip == '\0') {
        return;
    }

    pthread_mutex_lock(&peers_mutex);

    cur = find_by_ip_nolock(adip);
    if (cur != NULL) {
        trim_copy(cur->nom, sizeof(cur->nom), pseudo);
        pthread_mutex_unlock(&peers_mutex);
        return;
    }

    cur = find_by_pseudo_nolock(pseudo);
    if (cur != NULL) {
        trim_copy(cur->adip, sizeof(cur->adip), adip);
        pthread_mutex_unlock(&peers_mutex);
        return;
    }

    node = calloc(1, sizeof(*node));
    if (node == NULL) {
        pthread_mutex_unlock(&peers_mutex);
        perror("calloc");
        return;
    }

    trim_copy(node->nom, sizeof(node->nom), pseudo);
    trim_copy(node->adip, sizeof(node->adip), adip);

    prev = NULL;
    cur = liste;
    while (cur != NULL && strcmp(cur->nom, node->nom) < 0) {
        prev = cur;
        cur = cur->next;
    }

    if (prev == NULL) {
        node->next = liste;
        liste = node;
    } else {
        node->next = cur;
        prev->next = node;
    }

    pthread_mutex_unlock(&peers_mutex);
}

static void supprimeElt(char *adip)
{
    TRACE_PRINT("[TRACE] suppression ip=%s\n", adip);
    struct elt *cur;
    struct elt *prev;

    if (adip == NULL || *adip == '\0') {
        return;
    }

    pthread_mutex_lock(&peers_mutex);

    prev = NULL;
    cur = liste;
    while (cur != NULL) {
        if (strcmp(cur->adip, adip) == 0) {
            if (prev == NULL) {
                liste = cur->next;
            } else {
                prev->next = cur->next;
            }
            free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    pthread_mutex_unlock(&peers_mutex);
}

static void listeElts(void)
{
    struct elt *cur;
    int n = 0;

    pthread_mutex_lock(&peers_mutex);
    printf("\n=== Liste des presents ===\n");
    for (cur = liste; cur != NULL; cur = cur->next) {
        printf("- %-23s %s\n", cur->nom, cur->adip);
        ++n;
    }
    if (n == 0) {
        printf("(aucun participant connu)\n");
    }
    printf("==========================\n\n");
    pthread_mutex_unlock(&peers_mutex);
}

static int commande(char octet1, char *message, char *pseudo)
{
    if (!running || sid < 0) {
        errno = ENOTCONN;
        return -1;
    }

    switch (octet1) {
        case '3':
            listeElts();
            return 0;

        case '4': {
            struct in_addr dest;
            struct elt *cur;
            char ip[16];

            if (pseudo == NULL || message == NULL || *pseudo == '\0') {
                errno = EINVAL;
                return -1;
            }

            pthread_mutex_lock(&peers_mutex);
            cur = find_by_pseudo_nolock(pseudo);
            if (cur == NULL) {
                pthread_mutex_unlock(&peers_mutex);
                errno = ENOENT;
                return -1;
            }
            trim_copy(ip, sizeof(ip), cur->adip);
            pthread_mutex_unlock(&peers_mutex);

            if (ip_string_to_addr(ip, &dest) == -1) {
                return -1;
            }
            if (send_to_ip(dest, '9', message) == -1) {
                return -1;
            }
            printf("[PRIVE] message envoye a %s (%s)\n", pseudo, ip);
            return 0;
        }

        case '5': {
            struct elt *cur;
            struct in_addr dests[256];
            char pseudos[256][LPSEUDO + 1];
            int count = 0;
            int i;

            if (message == NULL) {
                errno = EINVAL;
                return -1;
            }

            pthread_mutex_lock(&peers_mutex);
            for (cur = liste; cur != NULL && count < 256; cur = cur->next) {
                struct in_addr a;
                if (ip_string_to_addr(cur->adip, &a) == -1) {
                    continue;
                }
                if (is_my_ip(a)) {
                    continue;
                }
                dests[count] = a;
                trim_copy(pseudos[count], sizeof(pseudos[count]), cur->nom);
                ++count;
            }
            pthread_mutex_unlock(&peers_mutex);

            for (i = 0; i < count; ++i) {
                if (send_to_ip(dests[i], '9', message) == -1) {
                    perror("sendto");
                } else {
                    printf("[DIFFUSION] message envoye a %s (%s)\n",
                           pseudos[i], addr_to_str(dests[i]));
                }
            }
            return 0;
        }

        default:
            errno = EINVAL;
            return -1;
    }
}

static void *serveur_udp(void *p)
{
    TRACE_PRINT("[TRACE] thread UDP lance, tid=%lu\n",(unsigned long)pthread_self());
    char *pseudo = (char *)p;
    struct sockaddr_in local_addr;
    struct sockaddr_in from;
    socklen_t from_len;
    int opt = 1;
    char buf[LBUF + 1];
    ssize_t n;
    

    trim_copy(my_pseudo, sizeof(my_pseudo), pseudo);

    if (collect_interfaces() == -1) {
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

    pthread_mutex_lock(&peers_mutex);
    videListe_nolock();
    pthread_mutex_unlock(&peers_mutex);

    {
        int i;
        for (i = 0; i < nb_my_ips; ++i) {
            char ip[16];
            trim_copy(ip, sizeof(ip), inet_ntoa(my_ips[i]));
            ajouteElt(my_pseudo, ip);
        }
    }

    printf("Serveur BEUIP (thread UDP) lance avec le pseudo '%s' sur le port %d\n",
           my_pseudo, BEUIP_PORT);
    if (nb_bcasts == 0) {
        printf("Aucune interface broadcast exploitable detectee\n");
    } else {
        int i;
        printf("Broadcasts detectes :\n");
        for (i = 0; i < nb_bcasts; ++i) {
            printf("- %s\n", addr_to_str(bcasts[i].addr));
        }
    }

    send_to_all_broadcasts('1', my_pseudo);
    printf("Annonce de presence envoyee sur les broadcasts detectes\n");

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
                if (payload_len > 0) {
                    char ip[16];
                    trim_copy(ip, sizeof(ip), inet_ntoa(from.sin_addr));
                    ajouteElt(payload, ip);
                    if (!is_my_ip(from.sin_addr)) {
                        (void)send_to_ip(from.sin_addr, '2', my_pseudo);
                    }
                }
                break;

            case '2':
                if (payload_len > 0) {
                    char ip[16];
                    trim_copy(ip, sizeof(ip), inet_ntoa(from.sin_addr));
                    ajouteElt(payload, ip);
                }
                break;

            case '9': {
                struct elt *cur;
                char src_ip[16];
                trim_copy(src_ip, sizeof(src_ip), inet_ntoa(from.sin_addr));

                pthread_mutex_lock(&peers_mutex);
                cur = find_by_ip_nolock(src_ip);
                if (cur != NULL) {
                    printf("Message de %s : %s\n", cur->nom, payload);
                } else {
                    printf("Message de %s : %s (pseudo inconnu)\n", src_ip, payload);
                }
                pthread_mutex_unlock(&peers_mutex);
                break;
            }

            case '0': {
                char ip[16];
                trim_copy(ip, sizeof(ip), inet_ntoa(from.sin_addr));
                supprimeElt(ip);
                break;
            }

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
        send_to_all_broadcasts('0', my_pseudo);
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
    videListe_nolock();
    pthread_mutex_unlock(&peers_mutex);

    nb_bcasts = 0;
    nb_my_ips = 0;
    my_pseudo[0] = '\0';

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
