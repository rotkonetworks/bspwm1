CFLAGS  += -std=c99 -D_POSIX_C_SOURCE=200809L -pedantic -Wall -Wextra

all: test_window test_window_wl

test_window: test_window.c
	$(CC) $(CFLAGS) -o $@ $< -lxcb -lxcb-icccm

test_window_wl: test_window_wl.c xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -o $@ test_window_wl.c xdg-shell-protocol.c -lwayland-client

clean:
	$(RM) test_window test_window_wl *.o

.PHONY: all clean
