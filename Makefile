# tbs5580 for macOS - user-space DVB-S/S2 driver (no external runtime deps
# beyond libusb and system ncurses/iconv)
CC      ?= clang
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS   := $(shell pkg-config --libs libusb-1.0)
PREFIX  ?= /usr/local

SRCS = src/main.c src/frontend.c src/tbs5580.c src/si2183.c src/av201x.c \
       src/tsout.c src/psi.c src/channels.c src/rotor.c
OBJS = $(SRCS:.c=.o)

tbs5580: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(USB_LIBS) -liconv -lncurses -lpthread

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) $(USB_CFLAGS) -c -o $@ $<

install: tbs5580
	install -d $(PREFIX)/bin $(PREFIX)/share/tbs5580
	install -m 755 tbs5580 $(PREFIX)/bin/
	install -m 644 firmware/*.fw $(PREFIX)/share/tbs5580/

clean:
	rm -f tbs5580 $(OBJS)

.PHONY: install clean
