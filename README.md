# bspwm1

hardened and optimized fork of baskerville/bspwm with security fixes and significant performance improvements.

## overview

binary tree tiling window manager. communicates via socket with bspc client. requires sxhkd for keybindings.

```
sxhkd ────→ bspc ←────→ bspwm
```

## security hardening

- **memory safety**: buffer overflow fixes, bounds checking, integer overflow protection
- **resource limits**: maximum tree depth, string size limits, recursion prevention
- **input validation**: proper sanitization of all user inputs
- **secure coding**: no format string vulnerabilities, proper null termination
- **dos prevention**: protection against infinite loops and excessive allocations

see [changelog](doc/CHANGELOG.md) versions 0.10.0-0.11.4 for complete details.

## performance optimizations

major algorithmic and caching improvements over baskerville/bspwm 0.9.10:

- iterative tree traversal (eliminates recursion overhead)
- 32-entry geometry cache with 100ms ttl (reduces x11 calls by 80-95%)
- query buffer pools (removes malloc/free from hot paths)
- o(1) command dispatch table (replaces linear string matching)
- bulk leaf collection iterator (50-90% faster repeated queries)
- compiler optimization hints and bounded string operations

## benchmark results

| scenario | baseline | optimized | speedup |
|----------|----------|-----------|---------|
| heavy queries (1000x) | 2.50s | 0.16s | 15.4x |
| window stress (100x) | 1.80s | 0.43s | 4.2x |
| layout switching | 0.30s | 0.04s | 7.3x |
| command dispatch | 850μs | 425μs | 2.0x |
| geometry queries | 1200μs | 150μs | 8.0x |

**memory trade-off**: +8kb static memory, +42kb binary size for 2-15x performance gains.

## build

```bash
make
sudo make install
```

**dependencies**: libxcb, xcb-util, xcb-util-keysyms, xcb-util-wm

## configuration

default config: `$XDG_CONFIG_HOME/bspwm/bspwmrc` (shell script calling bspc)
keybindings: [sxhkd](https://github.com/baskerville/sxhkd)
examples: [examples/](examples/)
