CC?=gcc
CFLAGS?=-std=c99 -g -Wall -Wextra -pedantic -DDEBUG
LIBS=-lreadline -ltermcap

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

all: lsh

lsh: parse.o lsh.o
	$(CC) $(CFLAGS) -o lsh parse.o lsh.o $(LIBS)

clean:
	-$(RM) parse.o lsh.o lsh
