#!/bin/bash
#
# Compare X11 latency between current and upstream bspwm
# Requires: Xvfb, both bspwm binaries built
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BSPWM_DIR="$(dirname "$SCRIPT_DIR")"

CURRENT_BSPWM="$BSPWM_DIR/bspwm"
UPSTREAM_BSPWM="$BSPWM_DIR/bspwm-0.9.12"
BENCH_BIN="$SCRIPT_DIR/x11_latency_bench"
EMPTY_RC="$SCRIPT_DIR/.empty_bspwmrc"

ITERATIONS="${1:-50}"
DISPLAY_NUM="${2:-99}"

cleanup() {
    jobs -p | xargs -r kill 2>/dev/null || true
    rm -f "/tmp/.X${DISPLAY_NUM}-lock" 2>/dev/null || true
    rm -f "$EMPTY_RC" 2>/dev/null || true
}
trap cleanup EXIT

build_bench() {
    echo "Building benchmark..."
    gcc -O2 -o "$BENCH_BIN" "$SCRIPT_DIR/x11_latency_bench.c" \
        -lxcb -lxcb-icccm -lxcb-ewmh 2>/dev/null || {
        echo "Failed to build benchmark. Install: libxcb-dev libxcb-icccm4-dev libxcb-ewmh-dev"
        exit 1
    }
    # Create empty bspwmrc to prevent loading user config
    echo '#!/bin/sh' > "$EMPTY_RC"
    echo 'bspc config border_width 2' >> "$EMPTY_RC"
    chmod +x "$EMPTY_RC"
}

check_binaries() {
    if [[ ! -x "$CURRENT_BSPWM" ]]; then
        echo "Current bspwm not found: $CURRENT_BSPWM"
        echo "Run 'make' first"
        exit 1
    fi
    if [[ ! -x "$UPSTREAM_BSPWM" ]]; then
        echo "Upstream bspwm not found: $UPSTREAM_BSPWM"
        echo "Build upstream 0.9.12 and copy to: $UPSTREAM_BSPWM"
        exit 1
    fi
}

run_benchmark() {
    local bspwm_bin="$1"
    local label="$2"

    cleanup
    echo '#!/bin/sh' > "$EMPTY_RC"
    echo 'bspc config border_width 2' >> "$EMPTY_RC"
    chmod +x "$EMPTY_RC"

    # Start Xvfb
    Xvfb ":${DISPLAY_NUM}" -screen 0 1920x1080x24 &>/dev/null &
    sleep 0.5

    export DISPLAY=":${DISPLAY_NUM}"

    # Start bspwm with empty config (no polybar, dunst, etc)
    "$bspwm_bin" -c "$EMPTY_RC" &>/dev/null &
    sleep 0.5

    # Run benchmark
    echo ""
    echo "=== $label ==="
    echo "Binary: $bspwm_bin"
    "$BENCH_BIN" "$ITERATIONS"

    cleanup
    sleep 0.3
}

main() {
    echo "X11 Latency Comparison Benchmark"
    echo "================================"
    echo "Iterations: $ITERATIONS"
    echo ""

    check_binaries
    build_bench

    # Warm up the system
    echo "Warming up..."
    sleep 1

    # Run benchmarks
    run_benchmark "$UPSTREAM_BSPWM" "UPSTREAM (bspwm 0.9.12)"
    run_benchmark "$CURRENT_BSPWM" "CURRENT (hardened fork)"

    echo ""
    echo "Done."
}

main
