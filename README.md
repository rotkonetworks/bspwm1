# bspwm

hardened fork of baskerville/bspwm. security fixes, memory safety improvements, reduced X11 latency.

## overview

binary tree tiling window manager. socket IPC with bspc client. requires sxhkd for keybindings.

## changes from upstream

### security hardening

- buffer overflow fixes with bounds checking
- integer overflow protection on size calculations
- safe memory allocation wrappers (NULL checks, overflow detection)
- format string vulnerability fixes
- input validation on all IPC commands
- resource limits (tree depth, string sizes)

### memory safety

- scratch arena allocator for per-command temporary allocations
- eliminates leak-prone manual free() on error paths
- `__attribute__((cold))` on error handlers for better I-cache utilization
- struct field reordering to eliminate padding holes

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

identical performance. security hardening adds no measurable overhead.

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
make BACKEND=wlroots WLROOTS_DIR=../wlroots   # Wayland compositor -> bspwm-wl
make BACKEND=wlroots install                  # installs bspwm-wl, bspc and the wayland-sessions entry
```

each backend is a separate build and a separate binary. objects go under `build/<backend>/`, so the two can be built and installed side by side. `bspc` is shared and links no display library. the wayland build has no sxhkd: keybindings are handled in-process via `bspc keybind`. github releases and the aur package build the X11 backend only.

dependencies (x11): libxcb, xcb-util, xcb-util-keysyms, xcb-util-wm, libxkbcommon
dependencies (wlroots): a wlroots 0.21 checkout at `WLROOTS_DIR`, wayland-server, libxkbcommon, pixman, libdrm

binary size: 285KB (+34KB / +13% vs upstream 0.9.12)

## configuration

config: `$XDG_CONFIG_HOME/bspwm/bspwmrc`
keybindings: sxhkd
examples: `examples/`

## license

BSD-2-Clause
