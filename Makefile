CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c99

all: termux-iso

termux-iso: termux_iso.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f termux-iso
