# bspwm Benchmarking Suite

This directory contains a comprehensive benchmarking suite for measuring bspwm performance and comparing it against upstream versions.

## Quick Start

```bash
# Run the complete automated benchmark suite
./run_benchmarks.sh
```

This will:
1. Build current and upstream (0.9.12) bspwm versions
2. Run microbenchmarks on both
3. Generate detailed performance comparison report
4. Save results for future analysis

## Manual Benchmarking

### Microbenchmarks

Test core algorithms and data structures:

```bash
# Build and run microbenchmarks
gcc -o microbench microbench.c -lm -lxcb
./microbench
```

**Tests included:**
- Tree traversal algorithms (recursive vs iterative)
- Window tree operations at different depths
- String operation performance
- Memory access patterns

### Integration Benchmarks

Test real bspwm operations (requires X session):

```bash
# Benchmark current version
python3 bench.py current

# Benchmark upstream version (after running automated suite)
python3 bench.py upstream

# Compare results
python3 bench.py compare

# Run everything
python3 bench.py all
```

**Tests included:**
- bspc query operations
- Layout switching performance
- Tree navigation commands
- Memory usage analysis

## Files Description

| File | Purpose |
|------|---------|
| `run_benchmarks.sh` | **Main automated benchmark runner** |
| `microbench.c` | Low-level microbenchmarks (CPU cycles) |
| `bench.py` | High-level integration benchmarks |
| `bspc_bench.py` | bspc command benchmarks |
| `syscall_bench.py` | System call performance testing |

## Requirements

### Dependencies
- `git`, `gcc`, `make`, `python3`
- XCB development libraries:
  ```bash
  # Debian/Ubuntu
  sudo apt install libxcb-dev libxcb-util-dev libxcb-keysyms1-dev \
                   libxcb-icccm4-dev libxcb-ewmh-dev libxcb-randr0-dev \
                   libxcb-xinerama0-dev libxcb-shape0-dev

  # Arch Linux
  sudo pacman -S libxcb xcb-util xcb-util-keysyms xcb-util-wm
  ```

### Optional (for integration tests)
- X11 session
- `xterm` (for window spawn/destroy tests)

## Understanding Results

### Microbenchmark Output

```
=== Tree Depth 8 ===
first_extrema (recursive)     :       43 ± 24 cycles (min:     24, max:    300)
first_extrema (iterative)     :       20 ± 10 cycles (min:      1, max:     50)
```

- **Lower cycles = better performance**
- `±` shows standard deviation (consistency)
- Compare recursive vs iterative implementations

### Integration Benchmark Output

```
query_nodes:
  Mean: 45.32μs ± 12.45μs
  P95:  67.12μs
  P99:  89.45μs
```

- **Lower microseconds = better performance**
- P95/P99 show tail latency (worst-case performance)

### Binary Size Analysis

```
| Binary | Current | Upstream | Difference | % Change |
|--------|---------|----------|------------|----------|
| bspwm  | 257K    | 228K     | +28K       | +12.6%   |
```

- Shows impact of security hardening and modern features
- Larger binaries may indicate more features/security

## Performance Optimization Tips

### For Developers

1. **Focus on hot paths**: Tree traversal and query operations
2. **Measure twice, optimize once**: Use these benchmarks to verify improvements
3. **Consider trade-offs**: Security vs performance, memory vs speed
4. **Test at scale**: Use deeper trees (depth 16+) for real-world scenarios

### For Users

1. **Window layout complexity**: Deep nested layouts may impact performance
2. **Query frequency**: Frequent `bspc query` calls in scripts can add overhead
3. **Memory usage**: Monitor RSS if running many windows

## Interpreting Comparisons

### Good Performance Indicators ✅
- Lower CPU cycles for core operations
- Consistent timing (low standard deviation)
- Better performance on complex scenarios (deep trees)
- Reasonable memory usage

### Performance Concerns ⚠️
- High variance in timing
- Degraded performance on complex layouts
- Excessive memory growth
- Slower query operations

## Adding New Benchmarks

### Microbenchmark (C)

1. Add test function in `microbench.c`
2. Create wrapper function for benchmark runner
3. Add call in `main()` function

```c
void bench_new_feature(void *data) {
    // Your benchmark code here
    volatile int result = new_feature_function(data);
    (void)result; // Prevent optimization
}
```

### Integration Benchmark (Python)

1. Add method to `BspwmBenchmark` class in `bench.py`
2. Add call in `run_benchmarks()` method

```python
def new_feature_test(self):
    """Test new feature performance"""
    try:
        # Your test code here
        subprocess.run(['./bspc', 'new-command'], ...)
        return True
    except:
        return False
```

## Troubleshooting

### Common Issues

**"Failed to build" errors**
- Check XCB library installation
- Verify gcc/make are available

**Integration benchmarks fail**
- Ensure X11 session is running
- Check if `xterm` is available
- Some tests require active bspwm session

**Permission errors**
- Make scripts executable: `chmod +x run_benchmarks.sh`

**Inconsistent results**
- CPU frequency scaling affects microbenchmarks
- Close other applications during testing
- Run multiple iterations and average results

## Continuous Performance Monitoring

### Automated Testing
```bash
# Add to CI/CD pipeline
./benches/run_benchmarks.sh > performance-results.log
```

### Performance Regression Detection
```bash
# Compare against baseline
python3 bench.py compare
# Check exit code for regression detection
```

### Historical Analysis
- Save benchmark results with version tags
- Track performance trends over time
- Identify performance regressions early

---

For questions or contributions to the benchmarking suite, please see the main project documentation.