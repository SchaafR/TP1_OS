/*****
* Etape 1 : serveur UDP avec accuse de reception
* Le serveur recoit un datagramme puis renvoie un AR au client.
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

#define PORT 9999
#define LBUF 512

char buf[LBUF+1];
struct sockaddr_in SockConf; /* pour les operations du serveur */

char *addrip(unsigned long A)
{
    static char b[16];
    sprintf(b, "%u.%u.%u.%u",
            (unsigned int)(A >> 24 & 0xFF),
            (unsigned int)(A >> 16 & 0xFF),
            (unsigned int)(A >> 8 & 0xFF),
            (unsigned int)(A & 0xFF));
    return b;
}

int main(int N, char *P[])
{
    int sid, n;
    struct sockaddr_in Sock;
    socklen_t ls;
    const char *ar = "Bien recu 5/5 !";

    (void)N;
    (void)P;

    /* creation du socket */
    if ((sid = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket");
        return 2;
    }

    /* initialisation de SockConf pour le bind */
    bzero(&SockConf, sizeof(SockConf));
    SockConf.sin_family = AF_INET;
    SockConf.sin_addr.s_addr = htonl(INADDR_ANY);
    SockConf.sin_port = htons(PORT);

    if (bind(sid, (struct sockaddr *)&SockConf, sizeof(SockConf)) == -1) {
        perror("bind");
        close(sid);
        return 3;
    }

    printf("Le serveur est attache au port %d !\n", PORT);

    for (;;) {
        ls = sizeof(Sock);

        /* on attend un message */
        if ((n = recvfrom(sid, (void *)buf, LBUF, 0,
                          (struct sockaddr *)&Sock, &ls)) == -1) {
            perror("recvfrom");
        } else {
            buf[n] = '\0';
            printf("recu de %s:%d : <%s>\n",
                   addrip(ntohl(Sock.sin_addr.s_addr)),
                   ntohs(Sock.sin_port),
                   buf);

            /* renvoi de l'accuse de reception au client */
            if (sendto(sid, ar, strlen(ar), MSG_CONFIRM,
                       (struct sockaddr *)&Sock, ls) == -1) {
                perror("sendto AR");
            } else {
                printf("AR envoye a %s:%d\n",
                       addrip(ntohl(Sock.sin_addr.s_addr)),
                       ntohs(Sock.sin_port));
            }
        }
    }

    close(sid);
    return 0;
}
