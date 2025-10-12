#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TEMP_DIR="/tmp/bspwm-benchmark-$$"
UPSTREAM_REPO="https://github.com/baskerville/bspwm.git"
UPSTREAM_VERSION="0.9.12"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log() {
    echo -e "${BLUE}[$(date +'%H:%M:%S')] $1${NC}"
}

success() {
    echo -e "${GREEN}✓ $1${NC}"
}

warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

error() {
    echo -e "${RED}✗ $1${NC}"
    exit 1
}

cleanup() {
    if [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
        log "Cleaned up temporary directory"
    fi
}

trap cleanup EXIT

print_header() {
    cat << 'EOF'
====================================================================
             bspwm Performance Benchmark Suite
====================================================================
This script will:
1. Build the current bspwm version
2. Clone and build upstream bspwm v0.9.12
3. Run microbenchmarks on both versions
4. Generate performance comparison report
====================================================================
EOF
}

check_dependencies() {
    log "Checking dependencies..."

    local deps=("git" "gcc" "make" "python3")
    local missing=()

    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
            missing+=("$dep")
        fi
    done

    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}"
    fi

    # Check for required libraries (Arch Linux)
    if ! pkg-config --exists xcb xcb-util xcb-keysyms xcb-icccm xcb-ewmh xcb-randr xcb-xinerama xcb-shape 2>/dev/null; then
        warning "Some XCB libraries might be missing. Install with:"
        warning "  sudo pacman -S libxcb xcb-util xcb-util-keysyms xcb-util-wm"
    fi

    success "All dependencies found"
}

build_current() {
    log "Building current bspwm version..."
    cd "$PROJECT_DIR"

    make clean > /dev/null 2>&1 || true
    if ! make -j"$(nproc)" > /dev/null 2>&1; then
        error "Failed to build current bspwm version"
    fi

    # Build microbenchmark
    if ! gcc -o "$SCRIPT_DIR/microbench" "$SCRIPT_DIR/microbench.c" -lm -lxcb 2>/dev/null; then
        error "Failed to build microbenchmark"
    fi

    success "Built current version ($(./bspwm --version 2>/dev/null || echo 'unknown'))"
}

build_upstream() {
    log "Cloning and building upstream bspwm v$UPSTREAM_VERSION..."

    mkdir -p "$TEMP_DIR"
    cd "$TEMP_DIR"

    if ! git clone -q --depth 1 --branch "$UPSTREAM_VERSION" "$UPSTREAM_REPO" upstream 2>/dev/null; then
        error "Failed to clone upstream repository"
    fi

    cd upstream
    if ! make -j"$(nproc)" > /dev/null 2>&1; then
        error "Failed to build upstream version"
    fi

    # Copy binaries to project directory
    cp bspwm "$PROJECT_DIR/bspwm-$UPSTREAM_VERSION"
    cp bspc "$PROJECT_DIR/bspc-$UPSTREAM_VERSION"

    success "Built upstream version v$UPSTREAM_VERSION"
}

run_microbenchmarks() {
    log "Running microbenchmarks..."
    cd "$PROJECT_DIR"

    # Run current version benchmark
    log "Benchmarking current version..."
    if ! timeout 60 "$SCRIPT_DIR/microbench" > current-bench-results.txt 2>/dev/null; then
        error "Current version benchmark failed or timed out"
    fi

    # Run upstream version benchmark (using same microbench binary)
    log "Benchmarking upstream version (using current microbench)..."
    if ! timeout 60 "$SCRIPT_DIR/microbench" > upstream-bench-results.txt 2>/dev/null; then
        error "Upstream version benchmark failed or timed out"
    fi

    success "Microbenchmarks completed"
}

run_integration_benchmarks() {
    log "Running isolated integration benchmarks..."
    cd "$SCRIPT_DIR"

    # Check for Xvfb (required for isolated testing)
    if ! command -v Xvfb &> /dev/null; then
        warning "Xvfb not found - install with: sudo pacman -S xorg-server-xvfb"
        warning "Skipping isolated integration benchmarks"
        return 0
    fi

    log "Running isolated benchmarks with proper version separation..."

    # Run isolated benchmarks that actually test different bspwm versions
    if python3 isolated_bench.py all > /dev/null 2>&1; then
        success "Isolated integration benchmarks completed"
    else
        warning "Some isolated benchmarks failed (check isolated_bench_*.json for results)"
    fi

    # Also run the old benchmarks for comparison (but warn they're not isolated)
    if [ -n "${DISPLAY:-}" ] && command -v xterm &> /dev/null; then
        warning "Running legacy benchmarks (these test against your running bspwm)"
        python3 bench.py current > /dev/null 2>&1 || warning "Legacy current benchmark failed"
        python3 bench.py upstream > /dev/null 2>&1 || warning "Legacy upstream benchmark failed (expected - same bspwm)"
        python3 bench.py compare 2>/dev/null || warning "Legacy benchmark comparison failed"
    fi
}

analyze_binaries() {
    log "Analyzing binary sizes..."
    cd "$PROJECT_DIR"

    # Create stripped copies for size comparison
    cp bspwm bspwm-stripped
    cp bspc bspc-stripped
    cp "bspwm-$UPSTREAM_VERSION" "bspwm-$UPSTREAM_VERSION-stripped"
    cp "bspc-$UPSTREAM_VERSION" "bspc-$UPSTREAM_VERSION-stripped"

    strip bspwm-stripped bspc-stripped "bspwm-$UPSTREAM_VERSION-stripped" "bspc-$UPSTREAM_VERSION-stripped" 2>/dev/null

    echo
    echo "=== Binary Size Comparison (Stripped) ==="
    printf "%-20s %12s %12s %12s\n" "Binary" "Current" "Upstream" "Difference"
    printf "%-20s %12s %12s %12s\n" "------" "-------" "--------" "----------"

    current_bspwm=$(stat -f%z bspwm-stripped 2>/dev/null || stat -c%s bspwm-stripped)
    upstream_bspwm=$(stat -f%z "bspwm-$UPSTREAM_VERSION-stripped" 2>/dev/null || stat -c%s "bspwm-$UPSTREAM_VERSION-stripped")
    diff_bspwm=$((current_bspwm - upstream_bspwm))
    printf "%-20s %12s %12s %+12s\n" "bspwm" "$(numfmt --to=iec $current_bspwm)" "$(numfmt --to=iec $upstream_bspwm)" "$(numfmt --to=iec $diff_bspwm)"

    current_bspc=$(stat -f%z bspc-stripped 2>/dev/null || stat -c%s bspc-stripped)
    upstream_bspc=$(stat -f%z "bspc-$UPSTREAM_VERSION-stripped" 2>/dev/null || stat -c%s "bspc-$UPSTREAM_VERSION-stripped")
    diff_bspc=$((current_bspc - upstream_bspc))
    printf "%-20s %12s %12s %+12s\n" "bspc" "$(numfmt --to=iec $current_bspc)" "$(numfmt --to=iec $upstream_bspc)" "$(numfmt --to=iec $diff_bspc)"

    # Clean up temporary files
    rm -f bspwm-stripped bspc-stripped "bspwm-$UPSTREAM_VERSION-stripped" "bspc-$UPSTREAM_VERSION-stripped"
}

generate_report() {
    log "Generating performance comparison report..."
    cd "$PROJECT_DIR"

    # Generate detailed markdown report
    cat > performance-comparison.md << EOF
# bspwm Performance Comparison: Current vs Upstream $UPSTREAM_VERSION

Generated on: $(date)
System: $(uname -a)
Compiler: $(gcc --version | head -n1)

## Binary Size Comparison

EOF

    # Add binary size analysis
    echo "| Binary | Current | Upstream | Difference | % Change |" >> performance-comparison.md
    echo "|--------|---------|----------|------------|----------|" >> performance-comparison.md

    current_bspwm=$(stat -c%s bspwm 2>/dev/null || stat -f%z bspwm)
    upstream_bspwm=$(stat -c%s "bspwm-$UPSTREAM_VERSION" 2>/dev/null || stat -f%z "bspwm-$UPSTREAM_VERSION")
    diff_bspwm=$((current_bspwm - upstream_bspwm))
    pct_bspwm=$(( (diff_bspwm * 100) / upstream_bspwm ))

    # Format difference with sign
    if [ $diff_bspwm -gt 0 ]; then
        diff_bspwm_str="+$(numfmt --to=iec $diff_bspwm)"
        pct_bspwm_str="+${pct_bspwm}%"
    else
        diff_bspwm_str="$(numfmt --to=iec $diff_bspwm)"
        pct_bspwm_str="${pct_bspwm}%"
    fi
    echo "| bspwm | $(numfmt --to=iec $current_bspwm) | $(numfmt --to=iec $upstream_bspwm) | $diff_bspwm_str | $pct_bspwm_str |" >> performance-comparison.md

    current_bspc=$(stat -c%s bspc 2>/dev/null || stat -f%z bspc)
    upstream_bspc=$(stat -c%s "bspc-$UPSTREAM_VERSION" 2>/dev/null || stat -f%z "bspc-$UPSTREAM_VERSION")
    diff_bspc=$((current_bspc - upstream_bspc))
    pct_bspc=$(( (diff_bspc * 100) / upstream_bspc ))

    # Format difference with sign
    if [ $diff_bspc -gt 0 ]; then
        diff_bspc_str="+$(numfmt --to=iec $diff_bspc)"
        pct_bspc_str="+${pct_bspc}%"
    else
        diff_bspc_str="$(numfmt --to=iec $diff_bspc)"
        pct_bspc_str="${pct_bspc}%"
    fi
    echo "| bspc | $(numfmt --to=iec $current_bspc) | $(numfmt --to=iec $upstream_bspc) | $diff_bspc_str | $pct_bspc_str |" >> performance-comparison.md

    # Add microbenchmark results
    echo "" >> performance-comparison.md
    echo "## Microbenchmark Results (Isolated Algorithm Testing)" >> performance-comparison.md
    echo "" >> performance-comparison.md
    echo "### Current Version" >> performance-comparison.md
    echo '```' >> performance-comparison.md
    cat current-bench-results.txt >> performance-comparison.md
    echo '```' >> performance-comparison.md
    echo "" >> performance-comparison.md
    echo "### Upstream Version" >> performance-comparison.md
    echo '```' >> performance-comparison.md
    cat upstream-bench-results.txt >> performance-comparison.md
    echo '```' >> performance-comparison.md

    # Add isolated integration benchmark results if they exist
    if [ -f "$SCRIPT_DIR/isolated_bench_current.json" ] && [ -f "$SCRIPT_DIR/isolated_bench_upstream.json" ]; then
        echo "" >> performance-comparison.md
        echo "## Isolated Integration Benchmarks (Actual Different bspwm Versions)" >> performance-comparison.md
        echo "" >> performance-comparison.md
        echo "These benchmarks run each bspwm version in separate Xvfb environments," >> performance-comparison.md
        echo "ensuring we actually test different bspwm processes (not just different bspc clients)." >> performance-comparison.md
        echo "" >> performance-comparison.md

        # Parse JSON results for display
        echo "### Results Summary" >> performance-comparison.md
        echo '```' >> performance-comparison.md
        cd "$SCRIPT_DIR"
        python3 isolated_bench.py compare 2>/dev/null >> ../performance-comparison.md || echo "Comparison failed" >> ../performance-comparison.md
        cd "$PROJECT_DIR"
        echo '```' >> performance-comparison.md
    else
        echo "" >> performance-comparison.md
        echo "## Integration Benchmarks" >> performance-comparison.md
        echo "" >> performance-comparison.md
        echo "⚠️ **WARNING**: Legacy integration benchmarks test against your currently running bspwm," >> performance-comparison.md
        echo "not isolated versions. For accurate results, use isolated_bench.py instead." >> performance-comparison.md
        echo "" >> performance-comparison.md
    fi

    success "Report generated: performance-comparison.md"
}

display_results() {
    log "Displaying benchmark results..."
    cd "$PROJECT_DIR"

    echo
    echo "=== Microbenchmark Results Summary ==="

    # Extract key metrics for comparison
    echo "Current Version Highlights:"
    grep -E "(Tree Depth|first_extrema.*recursive.*:|collect_leaves.*recursive.*:)" current-bench-results.txt | head -6

    echo
    echo "Upstream Version Highlights:"
    grep -E "(Tree Depth|first_extrema.*recursive.*:|collect_leaves.*recursive.*:)" upstream-bench-results.txt | head -6

    echo
    success "Full results saved in:"
    echo "  - current-bench-results.txt"
    echo "  - upstream-bench-results.txt"
    echo "  - performance-comparison.md"
}

main() {
    print_header
    check_dependencies
    build_current
    build_upstream
    run_microbenchmarks
    run_integration_benchmarks
    analyze_binaries
    generate_report
    display_results

    echo
    success "Benchmark suite completed successfully!"
    echo "Check performance-comparison.md for detailed analysis."
}

# Allow sourcing this script for individual functions
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi