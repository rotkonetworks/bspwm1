# bspwm

fork of baskerville/bspwm: a wlroots Wayland backend, edge snapping and magnetic edges, rule matching by property and regex, and fewer X11 round-trips.

## overview

binary tree tiling window manager. socket IPC with bspc client. on X11 it uses sxhkd for keybindings; the Wayland build binds keys itself.

## changes from upstream

### wayland backend

`bspwm-wl` is a wlroots 0.20 compositor running the same tree, rules and `bspc` protocol. it
supports layer shell, Xwayland, ext-workspace-v1 (waybar desktops), wlr-output-management
(wlr-randr, kanshi), primary selection and data-control. keybindings are in-process via
`bspc keybind`. see [build](#build).

### window management

- edge snapping: drag a window to a screen edge or corner, or `bspc node -S|--snap ZONE`
  (`edge_snap_enabled`, `edge_snap_threshold`)
- magnetic edges: dragged floating windows stick to work-area and window edges (`magnet_threshold`)
- per-desktop tile limit (`tile_limit_enabled`, `max_tiles_per_desktop`)
- rules match on `class`, `instance`, `name` and other properties, as regex with `~=`
  and case-insensitively with `/i`: `bspc rule -a 'class~=^Fire/i' state=floating`
- `ignore_monitor_updates` / `-i` freeze the monitor layout
- the control socket and state file live in `$XDG_RUNTIME_DIR`

### fixes to upstream behaviour

- **state restore** (`bspc wm -l`, `bspwm -r`): upstream sized the copy of monitor, desktop and
  node names from the JSON token length, so a long name overflowed `char[256]`. names are now
  clamped and token indices bounds-checked. JSON nesting is capped at 1024. the file must be a
  regular file or FIFO owned by the caller, at most 1 MiB
- **control socket**: a client that connected and sent nothing stalled the WM; sends and
  receives now time out after 2 s. the socket is mode 0600 with a `SO_PEERCRED` uid check, and
  bspwm won't take over a live instance's socket
- **argument parsing**: `parse_degree` / `parse_rectangle` use range-checked `strtol` instead of
  `atoi`/`%hu`. rules reject over-long consequences instead of truncating them
- **allocation**: NULL checks where upstream used `calloc` results directly (node and client
  creation, ewmh, restart argv)
- **geometry**: `rect_cmp` no longer truncates an unsigned area difference to `int`, and
  `adapt_geometry` uses 64-bit intermediates

### build

`-std=c23 -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection`,
full RELRO, warning-free under `-Wall -Wextra -Wformat=2 -Wvla -Wnull-dereference`.

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

single run against a bspwm 0.9.12 binary; it measures X11/IPC round-trips.

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

each backend is a separate build and a separate binary. objects go under `build/<backend>/`, so the two can be built and installed side by side. `bspc` is shared and links no display library. the wayland build has no sxhkd: keybindings are handled in-process via `bspc keybind`. bars: waybar with `ext/workspaces` for desktops and `wlr/taskbar` for windows. a compositor restart (`bspc wm -r`) closes every client, as on any Wayland compositor. github releases build the X11 backend; the AUR has `bspwm1` (X11) and `bspwm1-wl` (Wayland).

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
