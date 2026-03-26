#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "gescom.h"

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

static void traite_une_commande(char *commande) {
    if (analyseCom(commande) > 0) {
        if (!execComInt(getNMots(), getMots())) {
            execComExt(getMots());
        }
    }

    libereAnalyse();
}

static void (char *ligne) {
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

    /*
     * Le shell ignore Ctrl-C.
     */
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

        /*
         * Ctrl-D -> readline retourne NULL
         */
        if (ligne == NULL) {
            printf("\nSortie correcte de biceps.\n");
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