CC = cc
CFLAGS = -Wall -Wextra -pedantic -pthread
LDFLAGS = -lreadline

default: servudp_etape1 cliudp_etape1 servbeuip clibeuip biceps_P3

cliudp_etape1: cliudp_etape1.c
	$(CC) $(CFLAGS) -o cliudp_etape1 cliudp_etape1.c

servudp_etape1: servudp_etape1.c
	$(CC) $(CFLAGS) -o servudp_etape1 servudp_etape1.c

servbeuip: servbeuip.c
	$(CC) $(CFLAGS) -o servbeuip servbeuip.c

clibeuip: clibeuip.c
	$(CC) $(CFLAGS) -o clibeuip clibeuip.c

biceps_P3: biceps_P3.c creme_tp3_etape1.c gescom.c
	$(CC) $(CFLAGS) -o biceps_P3 biceps_P3.c creme_tp3_etape1.c gescom.c $(LDFLAGS)

clean:
	rm -f cliudp_etape1 servudp_etape1 servbeuip clibeuip biceps_P3 *.o