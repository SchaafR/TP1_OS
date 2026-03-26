#ifndef GESCOM_H
#define GESCOM_H

int analyseCom(char *b);
void libereAnalyse(void);

void majComInt(void);
void listeComInt(void);
int execComInt(int N, char **P);
int execComExt(char **P);

char **getMots(void);
int getNMots(void);

#endif