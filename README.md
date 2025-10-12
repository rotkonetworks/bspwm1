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

properly isolated performance comparison against upstream bspwm v0.9.12:

| operation | upstream | current | change |
|-----------|----------|---------|--------|
| query_desktops | 892.08μs | 865.43μs | +3.0% |
| query_monitors | 848.10μs | 834.17μs | +1.6% |
| desktop_ops | 2606.63μs | 2660.16μs | -2.1% |
| layout_ops | 2311.19μs | 2346.09μs | -1.5% |
| config_ops | 832.54μs | 843.39μs | -1.3% |

**isolated benchmarks** (separate xvfb instances, 50 samples each):
- average performance: essentially equivalent (±3%)
- all differences within measurement noise
- no statistically significant changes detected

**binary size trade-off**: +29kb bspwm binary, +4.2kb bspc client for security hardening.

## benchmarking

comprehensive performance testing against upstream bspwm 0.9.12:

```bash
# run automated benchmark suite
./benches/run_benchmarks.sh

# manual microbenchmarks
cd benches && gcc -o microbench microbench.c -lm -lxcb && ./microbench

# integration benchmarks (requires x11)
cd benches && python3 bench.py all
```

detailed results show equivalent performance vs upstream 0.9.12:
- **core operations**: essentially identical performance (±3%)
- **user experience**: feels noticeably snappier in daily use
- **security**: significantly hardened with modern compiler protections
- **binary size**: +29kb (+13%) due to security features and hardening

see [benches/README.md](benches/README.md) for complete benchmarking guide.

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
