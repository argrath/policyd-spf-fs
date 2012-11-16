# $Id: Makefile 24 2007-09-11 10:30:28Z cramer $

PREFIX	?= /usr/local
BIN     = $(DISTPATH)$(PREFIX)/bin
MAN	= $(DISTPATH)$(PREFIX)/share/man/man1

CC = gcc
CFLAGS = -g -O2 -Wall -DHAVE_GETOPT_LONG_ONLY
INCLUDE = -I /usr/include/spf2
LIBS = -lspf2 -lpthread -lnsl -lresolv

.PHONY: install
.PHONY: all
.PHONY: clean
.PHONY: install_restart

%.o:	%.c Makefile
	$(CC) $(CFLAGS) $(INCLUDE) -c -o $@ $<

all: policyd-spf-fs

policyd-spf-fs: policyd-spf-fs.o Makefile
	$(CC) $(CFLAGS) $(LIBS) policyd-spf-fs.o -o policyd-spf-fs

install: policyd-spf-fs policyd-spf-fs.1
	strip policyd-spf-fs
	install policyd-spf-fs $(BIN)
	install policyd-spf-fs.1 $(MAN)
	
install_restart:
	/etc/init.d/postfix stop
	make install
	/etc/init.d/postfix start

clean:
	rm -f *~ *.o policyd-spf-fs
	