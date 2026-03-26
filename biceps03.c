#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define HOSTNAME_MAX_LEN 256
#define NBMAXC 10

#ifdef TRACE
#define TRACE_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE_PRINT(...)
#endif

/* =========================
   Variables globales étape 2
   ========================= */

static char **Mots = NULL;   /* tableau des mots */
static int NMots = 0;        /* nombre de mots */

/* =========================
   Commandes internes étape 3
   ========================= */

typedef int (*fonction_commande)(int, char **);

typedef struct {
    char *nom;
    fonction_commande fonction;
} CommandeInterne;

static CommandeInterne Commandes[NBMAXC];
static int NbCommandesInternes = 0;

/* =========================
   Outils mémoire / prompt
   ========================= */

char *copyString(char *s) {
    char *copie;
    size_t taille;

    if (s == NULL) {
        return NULL;
    }

    taille = strlen(s) + 1;
    copie = malloc(taille);
    if (copie == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    strcpy(copie, s);
    return copie;
}

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

    fin_prompt = (geteuid() == 0) ? '#' : '$';

    /*
     * user + '@' + hostname + '$/#' + espace + '\0'
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

/* =========================
   Gestion de l'analyse
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

/*
 * Découpe la ligne selon espace, tab, newline.
 * Remplit Mots[] et NMots.
 * IMPORTANT étape 3 :
 * on réserve une case de plus et on met Mots[NMots] = NULL
 * pour pouvoir appeler execvp().
 */
int analyseCom(char *b) {
    char *buffer;
    char *curseur;
    char *mot;
    int capacite = 4;

    libereAnalyse();

    if (b == NULL) {
        return 0;
    }

    buffer = copyString(b);
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

        Mots[NMots] = copyString(mot);
        NMots++;
    }

    Mots[NMots] = NULL; /* indispensable pour execvp() */

    free(buffer);
    return NMots;
}

/* =========================
   Commandes internes
   ========================= */

int Sortie(int N, char *P[]) {
    (void)N;
    (void)P;
    exit(0);
}

void ajouteCom(char *nom, fonction_commande f) {
    if (NbCommandesInternes >= NBMAXC) {
        fprintf(stderr, "Erreur : tableau des commandes internes plein (NBMAXC=%d)\n", NBMAXC);
        exit(EXIT_FAILURE);
    }

    Commandes[NbCommandesInternes].nom = nom;
    Commandes[NbCommandesInternes].fonction = f;
    NbCommandesInternes++;
}

void majComInt(void) {
    NbCommandesInternes = 0;
    ajouteCom("exit", Sortie);
}

void listeComInt(void) {
    int i;

    printf("Commandes internes disponibles :\n");
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
            TRACE_PRINT("[TRACE] commande interne detectee : %s\n", P[0]);
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

    TRACE_PRINT("[TRACE] lancement commande externe : %s\n", P[0]);

    pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Fils */
        TRACE_PRINT("[TRACE] processus fils pid=%ld\n", (long)getpid());

        execvp(P[0], P);

        /* Si on arrive ici, execvp a échoué */
        fprintf(stderr, "Erreur : impossible d'executer '%s' : %s\n",
                P[0], strerror(errno));
        _exit(127);
    }

    /* Père */
    TRACE_PRINT("[TRACE] processus pere pid=%ld attend le fils pid=%ld\n",
                (long)getpid(), (long)pid);

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }

    if (WIFEXITED(status)) {
        TRACE_PRINT("[TRACE] le fils s'est termine avec le code %d\n",
                    WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        TRACE_PRINT("[TRACE] le fils a ete termine par le signal %d\n",
                    WTERMSIG(status));
    }

    return 0;
}

/* =========================
   Programme principal
   ========================= */

int main(void) {
    char *prompt;
    char *ligne;

    prompt = fabrique_prompt();

    majComInt();
    listeComInt();

    while (1) {
        ligne = readline(prompt);

        if (ligne == NULL) {
            printf("\n");
            break;
        }

        analyseCom(ligne);

        if (NMots > 0) {
            if (!execComInt(NMots, Mots)) {
                execComExt(Mots);
            }
        }

        free(ligne);
        libereAnalyse();
    }

    free(prompt);
    return 0;
}