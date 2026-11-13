CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic

rendezvous: rendezvous.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f rendezvous

.PHONY: clean
