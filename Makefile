# Makefile for fujiplay

CC = gcc
CPPFLAGS =
CFLAGS = -O2 -Wall
LDFLAGS = -s
SRCFILES = fujiplay.c yycc2ppm.c README Makefile fujiplay.lsm mx700-commands.html
LIBS =

all: fujiplay yycc2ppm
dist: fujiplay.tgz

clean:
	rm -f core *.o fujiplay yycc2ppm

fujiplay.tgz: $(SRCFILES)
	tar cvzf $@ $(SRCFILES)

fujiplay: fujiplay.o
	$(CC) $(LDFLAGS) -o $@ fujiplay.o $(LIBS)

yycc2ppm: yycc2ppm.o
	$(CC) $(LDFLAGS) -o $@ yycc2ppm.o $(LIBS)
