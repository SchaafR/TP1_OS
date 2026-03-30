default : servudp_etape1 cliudp_etape1 servbeuip clibeuip

cliudp_etape1 : cliudp_etape1.c
	cc -Wall -o cliudp_etape1 cliudp_etape1.c

servudp_etape1 : servudp_etape1.c
	cc -Wall -o servudp_etape1 servudp_etape1.c

servbeuip: servbeuip.c
	cc -Wall -Wextra -pedantic -o servbeuip servbeuip.c

clibeuip: clibeuip.c
	cc -Wall -Wextra -pedantic -o clibeuip clibeuip.c

clean :
	rm -f cliudp_etape1 servudp_etape1 servbeuip clibeuip

