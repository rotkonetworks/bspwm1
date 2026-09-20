CFLAGS  += -std=c99 -D_POSIX_C_SOURCE=200809L -pedantic -Wall -Wextra

all: test_window test_window_wl test_rule_match

test_window: test_window.c
	$(CC) $(CFLAGS) -o $@ $< -lxcb -lxcb-icccm

test_window_wl: test_window_wl.c xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -o $@ test_window_wl.c xdg-shell-protocol.c -lwayland-client

test_rule_match: test_rule_match.c ../src/rule_match.c ../src/rule_match.h
	$(CC) $(CFLAGS) -std=c23 -o $@ test_rule_match.c ../src/rule_match.c

clean:
	$(RM) test_window test_window_wl test_rule_match *.o

.PHONY: all clean
