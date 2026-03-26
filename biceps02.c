#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>

#define HOSTNAME_MAX_LEN 256

/* Variables globales demandées par le sujet */
static char **Mots = NULL;   /* tableau des mots */
static int NMots = 0;        /* nombre de mots */

/*
 * Copie dynamique d'une chaîne de caractères.
 * Équivalent simplifié de strdup(), mais écrit à la main.
 */
char *copyString(char *s) {
    char *copie;
    size_t taille;

    if (s == NULL) {
        return NULL;
    }

    taille = strlen(s) + 1; /* +1 pour le '\0' */
    copie = malloc(taille);
    if (copie == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    strcpy(copie, s);
    return copie;
}

/*
 * Libère le tableau global Mots et remet NMots à 0.
 */
void libereAnalyse(void) {
    int i;

    if (Mots != NULL) {
        for (i = 0; i < NMots; i++) {
            free(Mots[i]);
        }
        free(Mots);
    }

    Mots = NULL;
    NMots = 0;
}

/*
 * Analyse une ligne de commande.
 * Séparateurs : espace, tabulation, newline.
 * Remplit Mots et NMots.
 * Retourne le nombre de mots trouvés.
 */
int analyseCom(char *b) {
    char *tmp;
    char *mot;
    int capacite = 4; /* capacité initiale du tableau */

    /* On nettoie une éventuelle analyse précédente */
    libereAnalyse();

    if (b == NULL) {
        return 0;
    }

    tmp = copyString(b);

    Mots = malloc(capacite * sizeof(char *));
    if (Mots == NULL) {
        perror("malloc");
        free(tmp);
        exit(EXIT_FAILURE);
    }

    while ((mot = strsep(&tmp, " \t\n")) != NULL) {
        /*
         * strsep renvoie aussi des chaînes vides quand plusieurs
         * séparateurs se suivent. On les ignore.
         */
        if (*mot == '\0') {
            continue;
        }

        /* Agrandit le tableau si nécessaire */
        if (NMots >= capacite) {
            char **nouveau;

            capacite *= 2;
            nouveau = realloc(Mots, capacite * sizeof(char *));
            if (nouveau == NULL) {
                perror("realloc");
                free(tmp);
                libereAnalyse();
                exit(EXIT_FAILURE);
            }
            Mots = nouveau;
        }

        Mots[NMots] = copyString(mot);
        NMots++;
    }

    free(tmp);

    return NMots;
}

/*
 * Construit dynamiquement le prompt :
 * utilisateur@machine$  ou  utilisateur@machine#
 */
char *fabrique_prompt(void) {
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

    if (geteuid() == 0) {
        fin_prompt = '#';
    } else {
        fin_prompt = '$';
    }

    /*
     * user + '@' + hostname + '$' ou '#' + espace + '\0'
     */
    taille = strlen(user) + 1 + strlen(hostname) + 1 + 1 + 1;

    prompt = malloc(taille);
    if (prompt == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    snprintf(prompt, taille, "%s@%s%c ", user, hostname, fin_prompt);

    return prompt;
}

int main(void) {
    char *prompt;
    char *ligne;
    int i;

    prompt = fabrique_prompt();

    while (1) {
        ligne = readline(prompt);

        if (ligne == NULL) {
            printf("\n");
            break;
        }

        analyseCom(ligne);

        if (NMots == 0) {
            printf("Commande vide\n");
        } else {
            printf("Nom de la commande : %s\n", Mots[0]);

            if (NMots > 1) {
                printf("Parametres :\n");
                for (i = 1; i < NMots; i++) {
                    printf("  [%d] %s\n", i, Mots[i]);
                }
            } else {
                printf("Aucun parametre\n");
            }
        }

        free(ligne);
        libereAnalyse();
    }

    free(prompt);
    return 0;
}