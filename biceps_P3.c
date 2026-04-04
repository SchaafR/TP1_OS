#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "gescom.h"
#include "creme.h"

#define HOSTNAME_MAX_LEN 256
#define HISTFILE ".biceps_history"

static char *fabrique_prompt(void) {
    char hostname[HOSTNAME_MAX_LEN];
    char *user;
    char fin_prompt;
    size_t taille;
    char *prompt;

    user = getenv("USER");
    if (user == NULL) {
        user = "inconnu";
    }

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname");
        strcpy(hostname, "machine");
    }

    hostname[sizeof(hostname) - 1] = '\0';

    fin_prompt = (geteuid() == 0) ? '#' : '$';

    taille = strlen(user) + 1 + strlen(hostname) + 1 + 1 + 1;

    prompt = malloc(taille);
    if (prompt == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    snprintf(prompt, taille, "%s@%s%c ", user, hostname, fin_prompt);

    return prompt;
}

static char *fabrique_chemin_historique(void) {
    char *home;
    char *chemin;
    size_t taille;

    home = getenv("HOME");
    if (home == NULL) {
        return NULL;
    }

    taille = strlen(home) + 1 + strlen(HISTFILE) + 1;
    chemin = malloc(taille);
    if (chemin == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    snprintf(chemin, taille, "%s/%s", home, HISTFILE);
    return chemin;
}

static int ligne_utile(const char *s) {
    while (*s != '\0') {
        if (*s != ' ' && *s != '\t' && *s != '\n') {
            return 1;
        }
        s++;
    }
    return 0;
}

static int exec_commande_creme(int nmots, char **mots)
{
    if (nmots <= 0 || mots == NULL || mots[0] == NULL) {
        return 0;
    }

    if (strcmp(mots[0], "exit") == 0) {
        if (creme_server_pid() > 0) {
            if (creme_stop_server() == -1) {
                perror("creme_stop_server");
            }
        }
        exit(0);
    }

    if (strcmp(mots[0], "beuip") == 0) {
        if (nmots == 3 && strcmp(mots[1], "start") == 0) {
            if (creme_start_server(mots[2]) == -1) {
                perror("beuip start");
            }
            return 1;
        }

        if (nmots == 2 && strcmp(mots[1], "stop") == 0) {
            if (creme_stop_server() == -1) {
                perror("beuip stop");
            }
            return 1;
        }

        if (nmots == 2 && strcmp(mots[1], "help") == 0) {
            printf("Commandes BEUIP :\n");
            printf("  beuip start <pseudo>\n");
            printf("  beuip stop\n");
            printf("  beuip help\n");
            return 1;
        }

        fprintf(stderr, "Syntaxe invalide.\n");
        fprintf(stderr, "Usage : beuip start <pseudo> | beuip stop | beuip help\n");
        return 1;
    }

    if (strcmp(mots[0], "mess") == 0) {
        if (nmots == 2 && strcmp(mots[1], "list") == 0) {
            if (creme_list_peers() == -1) {
                perror("mess list");
            }
            return 1;
        }

        if (nmots >= 4 && strcmp(mots[1], "user") == 0) {
            size_t len = 0;
            char *message;
            int i;

            for (i = 3; i < nmots; ++i) {
                len += strlen(mots[i]) + 1;
            }

            message = malloc(len + 1);
            if (message == NULL) {
                perror("malloc");
                return 1;
            }

            message[0] = '\0';
            for (i = 3; i < nmots; ++i) {
                strcat(message, mots[i]);
                if (i + 1 < nmots) {
                    strcat(message, " ");
                }
            }

            if (creme_send_to_peer(mots[2], message) == -1) {
                perror("mess user");
            }

            free(message);
            return 1;
        }

        if (nmots >= 3 && strcmp(mots[1], "all") == 0) {
            size_t len = 0;
            char *message;
            int i;

            for (i = 2; i < nmots; ++i) {
                len += strlen(mots[i]) + 1;
            }

            message = malloc(len + 1);
            if (message == NULL) {
                perror("malloc");
                return 1;
            }

            message[0] = '\0';
            for (i = 2; i < nmots; ++i) {
                strcat(message, mots[i]);
                if (i + 1 < nmots) {
                    strcat(message, " ");
                }
            }

            if (creme_send_all(message) == -1) {
                perror("mess all");
            }

            free(message);
            return 1;
        }

        if (nmots == 2 && strcmp(mots[1], "help") == 0) {
            printf("Commandes mess :\n");
            printf("  mess list\n");
            printf("  mess user <pseudo> <message>\n");
            printf("  mess all <message>\n");
            printf("  mess help\n");
            return 1;
        }

        fprintf(stderr, "Syntaxe invalide.\n");
        fprintf(stderr, "Usage : mess list | mess user <pseudo> <message> | mess all <message> | mess help\n");
        return 1;
    }

    return 0;
}

static void traite_une_commande(char *commande) {
    if (analyseCom(commande) > 0) {
        if (!exec_commande_creme(getNMots(), getMots())) {
            if (!execComInt(getNMots(), getMots())) {
                execComExt(getMots());
            }
        }
    }

    libereAnalyse();
}

static void traite_ligne(char *ligne){
    char *buffer;
    char *curseur;
    char *commande;

    buffer = strdup(ligne);
    if (buffer == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    curseur = buffer;

    while ((commande = strsep(&curseur, ";")) != NULL) {
        if (!ligne_utile(commande)) {
            continue;
        }

        traite_une_commande(commande);
    }

    free(buffer);
}

int main(void) {
    char *prompt;
    char *ligne;
    char *histfile;

    signal(SIGINT, SIG_IGN);

    using_history();

    histfile = fabrique_chemin_historique();
    if (histfile != NULL) {
        read_history(histfile);
    }

    prompt = fabrique_prompt();

    majComInt();
    listeComInt();

    while (1) {
        ligne = readline(prompt);

        if (ligne == NULL) {
            printf("\nSortie correcte de biceps.\n");
            if (creme_server_pid() > 0) {
                if (creme_stop_server() == -1) {
                    perror("creme_stop_server");
                }
            }
            break;
        }

        if (ligne_utile(ligne)) {
            add_history(ligne);
            if (histfile != NULL) {
                write_history(histfile);
            }

            traite_ligne(ligne);
        }

        free(ligne);
    }

    free(prompt);
    free(histfile);
    return 0;
}