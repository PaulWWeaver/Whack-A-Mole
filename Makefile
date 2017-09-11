Whack-A-Mole: a.out
	cp a.out Whack-A-Mole
a.out: wam.c
	gcc -pthread -Wall -lrt -lncurses wam.c

install: Whack-A-Mole
	cp Whack-A-Mole /usr/bin

debug:
	gcc -g -O0 -pthread -Ddebug -D_GNU_SOURCE -Wall -lrt -lncurses -lefence wam.c

