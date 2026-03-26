CC = gcc
CFLAGS = -Wall
LDLIBS = -lreadline
OBJ = biceps04.o gescom.o

all: biceps01 biceps02 biceps03 biceps04

biceps01: biceps01.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

biceps02: biceps02.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

biceps03: biceps03.c
	$(CC) $(CFLAGS) -DTRACE -o $@ $< $(LDLIBS)

biceps04: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

biceps04.o: biceps04.c gescom.h
	$(CC) $(CFLAGS) -c biceps04.c

gescom.o: gescom.c gescom.h
	$(CC) $(CFLAGS) -c gescom.c

clean:
	rm -f biceps01 biceps02 biceps03 biceps04 *.o