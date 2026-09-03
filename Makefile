VERCMD  ?= git describe --tags 2> /dev/null
VERSION := $(shell $(VERCMD) || cat VERSION)
BACKEND ?= x11

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\"
CFLAGS   += -std=c23 -pedantic -Wall -Wextra -Wvla -Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2 -Wnull-dereference -Wstack-protector -fstack-protector-strong -fstack-clash-protection -fcf-protection -O2 -D_FORTIFY_SOURCE=3 -DJSMN_STRICT

# Core sources — backend-agnostic
CORE_SRC = bspwm.c helpers.c geometry.c jsmn.c settings.c monitor.c desktop.c tree.c stack.c history.c \
	 messages.c parse.c query.c restore.c rule.c subscribe.c keybind.c snap.c

# Backend selection. Each backend is a separate build with its own binary
# name, object directory and session file: the X11 build is `bspwm`, the
# wlroots compositor is `bspwm-wl`. They never share a process, and they can
# be installed side by side.
ifeq ($(BACKEND),x11)
    WM_BIN       = bspwm
    BACKEND_SRC  = backend_x11.c events.c pointer.c window.c ewmh.c
    BACKEND_LIBS = -lxcb -lxcb-util -lxcb-keysyms -lxcb-icccm -lxcb-ewmh -lxcb-randr -lxcb-xinerama -lxcb-shape -lxkbcommon
    CPPFLAGS += -DBACKEND_X11
    SESSION_FILE = contrib/freedesktop/bspwm.desktop
    SESSION_DIR  = $(XSESSIONS)
else ifeq ($(BACKEND),wlroots)
    WM_BIN       = bspwm-wl
    BACKEND_SRC  = backend_wlr.c window_ops.c
    CPPFLAGS += -DBACKEND_WLROOTS -DWLR_USE_UNSTABLE
    SESSION_FILE = contrib/freedesktop/bspwm-wayland.desktop
    SESSION_DIR  = $(WLSESSIONS)
    # Default: the distro's wlroots via pkg-config (Arch: wlroots0.20).
    # Set WLROOTS_DIR to a source checkout to build against its build/ dir
    # instead, e.g. for a wlroots-git development tree.
    WLROOTS_PC  ?= wlroots-0.20
    ifdef WLROOTS_DIR
        BACKEND_LIBS = -L$(WLROOTS_DIR)/build -lwlroots-$(WLROOTS_ABI)
        WLROOTS_ABI ?= 0.21
        CFLAGS += -I$(WLROOTS_DIR)/include -I$(WLROOTS_DIR)/build/include -I$(WLROOTS_DIR)/build/protocol
    else
        BACKEND_LIBS = $(shell pkg-config --libs $(WLROOTS_PC))
        CFLAGS += $(shell pkg-config --cflags $(WLROOTS_PC))
    endif
    BACKEND_LIBS += $(shell pkg-config --libs wayland-server xkbcommon 2>/dev/null)
    CFLAGS += $(shell pkg-config --cflags wayland-server libdrm pixman-1 xkbcommon 2>/dev/null)
    # wlroots' public headers include generated protocol headers that the
    # compositor is expected to produce itself (wayland-protocols and
    # wlr-protocols XML through wayland-scanner).
    PROTO_DIR  = build/$(BACKEND)/protocol
    PROTO_HDRS = $(PROTO_DIR)/wlr-layer-shell-unstable-v1-protocol.h \
                 $(PROTO_DIR)/wlr-output-power-management-unstable-v1-protocol.h
    CFLAGS += -I$(PROTO_DIR)
    WLR_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wlr-protocols 2>/dev/null || echo /usr/share/wlr-protocols)
else
    $(error Unknown BACKEND=$(BACKEND). Use x11 or wlroots)
endif

LDFLAGS  ?=
LDLIBS    = $(LDFLAGS) -lm $(BACKEND_LIBS)

PREFIX    ?= /usr/local
BINPREFIX ?= $(PREFIX)/bin
MANPREFIX ?= $(PREFIX)/share/man
DOCPREFIX ?= $(PREFIX)/share/doc/bspwm
BASHCPL   ?= $(PREFIX)/share/bash-completion/completions
FISHCPL   ?= $(PREFIX)/share/fish/vendor_completions.d
ZSHCPL    ?= $(PREFIX)/share/zsh/site-functions

MD_DOCS    = README.md doc/CHANGELOG.md doc/CONTRIBUTING.md doc/INSTALL.md doc/MISC.md doc/TODO.md
XSESSIONS  ?= $(PREFIX)/share/xsessions
WLSESSIONS ?= $(PREFIX)/share/wayland-sessions
CONFPREFIX ?= $(DESTDIR)$(HOME)/.config

# Objects live under build/<backend>/ so switching BACKEND can never link
# objects compiled with the other backend's defines (bspwm.c, rule.c and
# helpers.h all carry BACKEND_* ifdefs). Header deps come from the compiler.
OBJDIR   = build/$(BACKEND)
WM_SRC   = $(CORE_SRC) $(BACKEND_SRC)
WM_OBJ  := $(addprefix $(OBJDIR)/,$(WM_SRC:.c=.o))
CLI_SRC  = bspc.c helpers.c
CLI_OBJ := $(addprefix $(OBJDIR)/,$(CLI_SRC:.c=.o))

# bspc parses DISPLAY itself; it needs no X or Wayland library.
CLI_LIBS =

all: $(WM_BIN) bspc

debug: CFLAGS += -O0 -g
debug: $(WM_BIN) bspc

$(PROTO_DIR)/%-protocol.h: $(WLR_PROTOCOLS_DIR)/unstable/%.xml
	@mkdir -p $(PROTO_DIR)
	wayland-scanner server-header $< $@

$(OBJDIR)/%.o: src/%.c Makefile $(PROTO_HDRS)
	@mkdir -p $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(WM_OBJ:.o=.d) $(CLI_OBJ:.o=.d)

$(WM_BIN): $(WM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

bspc: LDLIBS = $(CLI_LIBS)
bspc: $(CLI_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Installs the binary and session file for the backend just built, nothing
# else. An X11-only build must not leave a wayland-sessions entry behind (and
# vice versa): a display manager would offer a session that cannot start.
install:
	mkdir -p "$(DESTDIR)$(BINPREFIX)"
	cp -pf $(WM_BIN) "$(DESTDIR)$(BINPREFIX)"
	cp -pf bspc "$(DESTDIR)$(BINPREFIX)"
	mkdir -p "$(DESTDIR)$(MANPREFIX)"/man1
	cp -p doc/bspwm.1 "$(DESTDIR)$(MANPREFIX)"/man1
	cp -Pp doc/bspc.1 "$(DESTDIR)$(MANPREFIX)"/man1
	mkdir -p "$(DESTDIR)$(BASHCPL)"
	cp -p contrib/bash_completion "$(DESTDIR)$(BASHCPL)"/bspc
	mkdir -p "$(DESTDIR)$(FISHCPL)"
	cp -p contrib/fish_completion "$(DESTDIR)$(FISHCPL)"/bspc.fish
	mkdir -p "$(DESTDIR)$(ZSHCPL)"
	cp -p contrib/zsh_completion "$(DESTDIR)$(ZSHCPL)"/_bspc
	mkdir -p "$(DESTDIR)$(DOCPREFIX)"
	cp -p $(MD_DOCS) "$(DESTDIR)$(DOCPREFIX)"
	mkdir -p "$(DESTDIR)$(DOCPREFIX)"/examples
	cp -pr examples/* "$(DESTDIR)$(DOCPREFIX)"/examples
	mkdir -p "$(DESTDIR)$(SESSION_DIR)"
	cp -p $(SESSION_FILE) "$(DESTDIR)$(SESSION_DIR)"

install_cfg:
	mkdir -p "$(CONFPREFIX)"/bspwm
	mkdir -p "$(CONFPREFIX)"/sxhkd
	cp -p examples/bspwmrc "$(CONFPREFIX)"/bspwm/bspwmrc
	cp -p examples/sxhkdrc "$(CONFPREFIX)"/sxhkd/sxhkdrc
	chmod +x "$(CONFPREFIX)"/bspwm/bspwmrc

uninstall:
	rm -f "$(DESTDIR)$(BINPREFIX)"/$(WM_BIN)
	rm -f "$(DESTDIR)$(BINPREFIX)"/bspc
	rm -f "$(DESTDIR)$(MANPREFIX)"/man1/bspwm.1
	rm -f "$(DESTDIR)$(MANPREFIX)"/man1/bspc.1
	rm -f "$(DESTDIR)$(BASHCPL)"/bspc
	rm -f "$(DESTDIR)$(FISHCPL)"/bspc.fish
	rm -f "$(DESTDIR)$(ZSHCPL)"/_bspc
	rm -rf "$(DESTDIR)$(DOCPREFIX)"
	rm -f "$(DESTDIR)$(SESSION_DIR)"/$(notdir $(SESSION_FILE))

test: $(WM_BIN) bspc
	@cd tests && $(MAKE) && ./run_headless $(BACKEND)

doc:
	a2x -v -d manpage -f manpage -a revnumber=$(VERSION) doc/bspwm.1.asciidoc

clean:
	rm -rf build bspwm bspwm-wl bspc

.PHONY: all debug install install_cfg uninstall doc clean test
