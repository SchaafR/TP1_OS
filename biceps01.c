#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>

#define HOSTNAME_MAX_LEN 256

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

    taille = strlen(user)      /* longueur du nom utilisateur */
       + 1                 /* '@' */
       + strlen(hostname)  /* longueur du nom machine */
       + 1                 /* '$' ou '#' */
       + 1                 /* espace */
       + 1;                /* '\0' */
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

    prompt = fabrique_prompt();

    while (1) {
        ligne = readline(prompt);

        /*
         * Si readline retourne NULL, cela signifie EOF
         * (par exemple Ctrl-D). Ici on quitte proprement.
         */
        if (ligne == NULL) {
            printf("\n");
            break;
        }

        /*
         * Étape 1 : on se contente d'afficher ce que l'utilisateur a tapé.
         */
        printf("Vous avez saisi : %s\n", ligne);

        free(ligne);
    }

    free(prompt);
    return 0;
}