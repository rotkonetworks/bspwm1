# bspwm

fork of baskerville/bspwm with an IPC and state-restore hardening pass, a wlroots backend and reduced X11 latency.

## overview

binary tree tiling window manager. socket IPC with bspc client. requires sxhkd for keybindings.

## changes from upstream

### hardening

fixes for issues reachable from input bspwm does not fully control:

- **state file restore** (`bspc wm -l`, `bspwm -r`): monitor/desktop/node name tokens
  are clamped to their buffers (upstream sized the copy from the token length and could
  overflow `char[256]`); token indices are bounds-checked; JSON nesting is capped at 1024;
  the file must be a regular file owned by the caller, under 1 MiB, and is NUL-terminated.
  the default state path is in `$XDG_RUNTIME_DIR`
- **control socket**: mode 0600, `SO_PEERCRED` uid check, 2 s send/receive timeouts (upstream's
  single-threaded loop could be stalled by a client that connects and sends nothing), and
  refusal to replace a live bspwm's socket
- **argument parsing**: `parse_degree`/`parse_rectangle` use range-checked `strtol` instead of
  `atoi`/`%hu`; rules reject over-long consequences instead of truncating; at most 256 arguments
  per message
- **NULL checks on allocation** where upstream dereferenced `calloc` results directly
  (node/client creation, ewmh, restart argv)
- **geometry**: `rect_cmp` no longer truncates an unsigned area difference to `int`;
  `adapt_geometry` uses 64-bit intermediates

the rest of the bounds checks in the fork guard code that was already correct upstream; they are
not security fixes. upstream had no format string bugs (every format argument was a literal).

### build

- `-std=c23 -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection`,
  full RELRO (`-z relro -z now`), `-Wformat=2 -Wvla -Wnull-dereference` and friends, warning-free.
  `-fcf-protection` only emits CET markers; enforcement depends on CPU and kernel

### memory

- small per-command scratch arena for the IPC argument vector and `query` selectors
- `__attribute__((cold))` on `warn()`/`err()`

### X11 pipelining

reduced X11 round-trips in rule application (6→1) and client initialization (4→1). benefit depends on X11 latency:

- **localhost**: ~10μs per round-trip, pipelining saves ~50μs (not measurable)
- **remote X11**: ~500μs per round-trip, pipelining saves ~2-4ms per window

### benchmark results (localhost, 100 iterations)

```
                      upstream    current
command latency:         4 μs       4 μs
window create+map:    1634 μs    1637 μs
```

single run against a bspwm 0.9.12 binary; it measures X11/IPC round-trips, not the hardened code paths.

## benchmarking

actual X11 latency comparison (requires Xvfb):

```bash
cd benches
./compare_latency.sh [iterations]
```

measures real IPC round-trip and window creation latency. this is what matters.

CPU cycle microbenchmarks exist in `benches/microbench.c` but are UX-irrelevant. 4000 cycles = 1.3μs. one X11 round-trip = 500μs.

## build

```bash
make                  # X11 backend -> bspwm
make install          # installs bspwm, bspc and the xsessions entry
```

```bash
make BACKEND=wlroots           # Wayland compositor -> bspwm-wl, against the distro wlroots (pkg-config wlroots-0.20)
make BACKEND=wlroots install   # installs bspwm-wl, bspc and the wayland-sessions entry
make BACKEND=wlroots WLROOTS_DIR=../wlroots WLROOTS_ABI=0.21   # or against a wlroots source checkout
```

each backend is a separate build and a separate binary. objects go under `build/<backend>/`, so the two can be built and installed side by side. `bspc` is shared and links no display library. the wayland build has no sxhkd: keybindings are handled in-process via `bspc keybind`. bars: waybar with `ext/workspaces` for desktops and `wlr/taskbar` for windows. a compositor restart (`bspc wm -r`) closes every client, as on any Wayland compositor. github releases and the aur package build the X11 backend only.

dependencies (x11): libxcb, xcb-util, xcb-util-keysyms, xcb-util-wm, libxkbcommon
dependencies (wlroots): wlroots 0.20, wayland, wayland-protocols, wlr-protocols, libxkbcommon, pixman, libdrm, xorg-xwayland

arch packages: `pkg/arch/PKGBUILD` (bspwm1, X11) and `pkg/arch/bspwm1-wl/PKGBUILD` (bspwm1-wl, Wayland)

binary size: 285KB (+34KB / +13% vs upstream 0.9.12)

## configuration

config: `$XDG_CONFIG_HOME/bspwm/bspwmrc`
keybindings: sxhkd
examples: `examples/`

## license

BSD-2-Clause
