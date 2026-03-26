#include "gescom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>

#define NBMAXC 20

#ifdef TRACE
#define TRACE_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE_PRINT(...)
#endif

static char **Mots = NULL;
static int NMots = 0;

typedef int (*fonction_commande)(int, char **);

typedef struct {
    char *nom;
    fonction_commande fonction;
} CommandeInterne;

static CommandeInterne Commandes[NBMAXC];
static int NbCommandesInternes = 0;

/* =========================
   Accesseurs
   ========================= */

char **getMots(void) {
    return Mots;
}

int getNMots(void) {
    return NMots;
}

/* =========================
   Analyse de commande
   ========================= */

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

int analyseCom(char *b) {
    char *buffer;
    char *curseur;
    char *mot;
    int capacite = 4;

    libereAnalyse();

    if (b == NULL) {
        return 0;
    }

    buffer = strdup(b);
    if (buffer == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    curseur = buffer;

    Mots = malloc((capacite + 1) * sizeof(char *));
    if (Mots == NULL) {
        perror("malloc");
        free(buffer);
        exit(EXIT_FAILURE);
    }

    while ((mot = strsep(&curseur, " \t\n")) != NULL) {
        if (*mot == '\0') {
            continue;
        }

        if (NMots >= capacite) {
            char **nouveau;

            capacite *= 2;
            nouveau = realloc(Mots, (capacite + 1) * sizeof(char *));
            if (nouveau == NULL) {
                perror("realloc");
                free(buffer);
                libereAnalyse();
                exit(EXIT_FAILURE);
            }
            Mots = nouveau;
        }

        Mots[NMots] = strdup(mot);
        if (Mots[NMots] == NULL) {
            perror("strdup");
            free(buffer);
            libereAnalyse();
            exit(EXIT_FAILURE);
        }

        NMots++;
    }

    Mots[NMots] = NULL;

    free(buffer);
    return NMots;
}

/* =========================
   Commandes internes
   ========================= */

static int Sortie(int N, char **P) {
    (void)N;
    (void)P;
    exit(0);
}

static int ChangeRep(int N, char **P) {
    char *dest;

    if (N < 2) {
        dest = getenv("HOME");
        if (dest == NULL) {
            fprintf(stderr, "cd: HOME non defini\n");
            return 1;
        }
    } else {
        dest = P[1];
    }

    if (chdir(dest) != 0) {
        perror("cd");
    }

    return 1;
}

static int AffichePwd(int N, char **P) {
    char cwd[PATH_MAX];

    (void)N;
    (void)P;

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return 1;
    }

    printf("%s\n", cwd);
    return 1;
}

static int Version(int N, char **P) {
    (void)N;
    (void)P;

    printf("biceps version 1.0\n");
    return 1;
}

static void ajouteCom(char *nom, fonction_commande f) {
    if (NbCommandesInternes >= NBMAXC) {
        fprintf(stderr, "Erreur : trop de commandes internes\n");
        exit(EXIT_FAILURE);
    }

    Commandes[NbCommandesInternes].nom = nom;
    Commandes[NbCommandesInternes].fonction = f;
    NbCommandesInternes++;
}

void majComInt(void) {
    NbCommandesInternes = 0;

    ajouteCom("exit", Sortie);
    ajouteCom("cd", ChangeRep);
    ajouteCom("pwd", AffichePwd);
    ajouteCom("vers", Version);
}

void listeComInt(void) {
    int i;

    printf("Commandes internes :\n");
    for (i = 0; i < NbCommandesInternes; i++) {
        printf("  - %s\n", Commandes[i].nom);
    }
}

int execComInt(int N, char **P) {
    int i;

    if (N <= 0 || P == NULL || P[0] == NULL) {
        return 0;
    }

    for (i = 0; i < NbCommandesInternes; i++) {
        if (strcmp(P[0], Commandes[i].nom) == 0) {
            TRACE_PRINT("[TRACE] commande interne : %s\n", P[0]);
            Commandes[i].fonction(N, P);
            return 1;
        }
    }

    return 0;
}

/* =========================
   Commandes externes
   ========================= */

int execComExt(char **P) {
    pid_t pid;
    int status;

    if (P == NULL || P[0] == NULL) {
        return -1;
    }

    TRACE_PRINT("[TRACE] commande externe : %s\n", P[0]);

    pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /*
         * Dans le fils, on remet Ctrl-C à son comportement par défaut.
         * Ainsi Ctrl-C interrompt la commande lancée, pas le shell parent.
         */
        signal(SIGINT, SIG_DFL);

        execvp(P[0], P);

        fprintf(stderr, "Erreur: execution impossible de '%s' : %s\n",
                P[0], strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }

    return 0;
}