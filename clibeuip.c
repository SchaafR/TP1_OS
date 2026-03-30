/*****
* Etape 2 : client local BEUIP
* Envoie les commandes au serveur local sur 127.0.0.1:9998
*
* Utilisation :
*   ./clibeuip liste
*   ./clibeuip msg pseudo message
*   ./clibeuip all message
*****/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BEUIP_PORT 9998
#define BEUIP_MAGIC "BEUIP"
#define BEUIP_MAGIC_LEN 5
#define LBUF 512

static ssize_t build_message_code3(char *buf, size_t buf_size)
{
    if (buf_size < 1 + BEUIP_MAGIC_LEN) {
        return -1;
    }
    buf[0] = '3';
    memcpy(buf + 1, BEUIP_MAGIC, BEUIP_MAGIC_LEN);
    return 1 + BEUIP_MAGIC_LEN;
}

static ssize_t build_message_code5(const char *message, char *buf, size_t buf_size)
{
    size_t len = strlen(message);
    size_t total = 1 + BEUIP_MAGIC_LEN + len;
    if (total > buf_size) {
        return -1;
    }
    buf[0] = '5';
    memcpy(buf + 1, BEUIP_MAGIC, BEUIP_MAGIC_LEN);
    memcpy(buf + 1 + BEUIP_MAGIC_LEN, message, len);
    return (ssize_t)total;
}

static ssize_t build_message_code4(const char *pseudo, const char *message, char *buf, size_t buf_size)
{
    size_t pseudo_len = strlen(pseudo);
    size_t message_len = strlen(message);
    size_t total = 1 + BEUIP_MAGIC_LEN + pseudo_len + 1 + message_len;

    if (total > buf_size) {
        return -1;
    }

    buf[0] = '4';
    memcpy(buf + 1, BEUIP_MAGIC, BEUIP_MAGIC_LEN);
    memcpy(buf + 1 + BEUIP_MAGIC_LEN, pseudo, pseudo_len);
    buf[1 + BEUIP_MAGIC_LEN + pseudo_len] = '\0';
    memcpy(buf + 1 + BEUIP_MAGIC_LEN + pseudo_len + 1, message, message_len);
    return (ssize_t)total;
}

int main(int argc, char *argv[])
{
    int sid;
    struct sockaddr_in dst;
    char buf[LBUF + 1];
    ssize_t n = -1;

    if (argc < 2) {
        fprintf(stderr,
                "Utilisation :\n"
                "  %s liste\n"
                "  %s msg pseudo message\n"
                "  %s all message\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    sid = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sid < 0) {
        perror("socket");
        return 2;
    }

    bzero(&dst, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);
    dst.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (strcmp(argv[1], "liste") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Utilisation : %s liste\n", argv[0]);
            close(sid);
            return 3;
        }
        n = build_message_code3(buf, sizeof(buf));
    } else if (strcmp(argv[1], "msg") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Utilisation : %s msg pseudo message\n", argv[0]);
            close(sid);
            return 4;
        }
        n = build_message_code4(argv[2], argv[3], buf, sizeof(buf));
    } else if (strcmp(argv[1], "all") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Utilisation : %s all message\n", argv[0]);
            close(sid);
            return 5;
        }
        n = build_message_code5(argv[2], buf, sizeof(buf));
    } else {
        fprintf(stderr, "Commande inconnue : %s\n", argv[1]);
        close(sid);
        return 6;
    }

    if (n < 0) {
        fprintf(stderr, "Message trop long\n");
        close(sid);
        return 7;
    }

    if (sendto(sid, buf, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst)) == -1) {
        perror("sendto");
        close(sid);
        return 8;
    }

    printf("Commande envoyee au serveur local 127.0.0.1:%d\n", BEUIP_PORT);
    close(sid);
    return 0;
}
