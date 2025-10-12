#!/usr/bin/env python3

import subprocess
import time
import statistics
import sys
import os
import json
import tempfile
import shutil
from pathlib import Path
from contextlib import contextmanager

class OptimizationBenchmark:
    """Benchmark the actual optimizations in bspwm v1.0.1"""

    def __init__(self):
        self.iterations = 100  # More iterations for statistical significance
        self.warmup_iterations = 10
        self.results = {}
        self.cleanup_pids = []

    def run_with_stats(self, name, test_func, env, bspc_binary):
        """Run benchmark with proper statistical analysis"""
        print(f"    Benchmarking {name}...")

        # Warmup
        for _ in range(self.warmup_iterations):
            try:
                test_func(env, bspc_binary)
            except:
                pass

        # Actual measurements
        times = []
        successful_runs = 0

        for i in range(self.iterations):
            try:
                start = time.perf_counter_ns()
                result = test_func(env, bspc_binary)
                end = time.perf_counter_ns()

                if result is not False:
                    times.append(end - start)
                    successful_runs += 1

            except Exception:
                pass

        if not times:
            print(f"    ✗ All benchmark iterations failed for {name}")
            return None

        # Convert to microseconds
        times_us = [t / 1000 for t in times]

        stats = {
            'min': min(times_us),
            'max': max(times_us),
            'mean': statistics.mean(times_us),
            'median': statistics.median(times_us),
            'stdev': statistics.stdev(times_us) if len(times_us) > 1 else 0,
            'p95': sorted(times_us)[int(0.95 * len(times_us))] if times_us else 0,
            'p99': sorted(times_us)[int(0.99 * len(times_us))] if times_us else 0,
            'samples': len(times_us),
            'success_rate': successful_runs / self.iterations
        }

        self.results[name] = stats

        print(f"    Mean: {stats['mean']:.1f}μs ± {stats['stdev']:.1f}μs "
              f"(P95: {stats['p95']:.1f}μs, success: {stats['success_rate']:.1%})")

        return stats

    # Test geometry cache performance
    def test_geometry_queries_burst(self, env, bspc_binary):
        """Test rapid geometry queries that should benefit from caching"""
        commands = [
            [bspc_binary, 'query', '-T', '-d'],  # Tree query (geometry heavy)
            [bspc_binary, 'query', '-N'],  # Node queries
            [bspc_binary, 'query', '-D'],  # Desktop queries
            [bspc_binary, 'query', '-M'],  # Monitor queries
        ]

        try:
            for cmd in commands:
                result = subprocess.run(cmd, env=env, capture_output=True, timeout=1)
                if result.returncode != 0:
                    return False
            return True
        except subprocess.TimeoutExpired:
            return False

    def test_repeated_geometry_queries(self, env, bspc_binary):
        """Test the same geometry query multiple times (cache hit)"""
        try:
            # First query (cache miss)
            result1 = subprocess.run([bspc_binary, 'query', '-T', '-d'],
                                   env=env, capture_output=True, timeout=1)
            # Second query (should be cache hit)
            result2 = subprocess.run([bspc_binary, 'query', '-T', '-d'],
                                   env=env, capture_output=True, timeout=1)

            return result1.returncode == 0 and result2.returncode == 0
        except subprocess.TimeoutExpired:
            return False

    # Test query buffer pools
    def test_bulk_queries(self, env, bspc_binary):
        """Test bulk queries that should benefit from buffer pools"""
        queries = [
            [bspc_binary, 'query', '-D'],  # Desktop queries
            [bspc_binary, 'query', '-M'],  # Monitor queries
            [bspc_binary, 'query', '-N'],  # Node queries
            [bspc_binary, 'query', '-N', '-d', 'focused'],
            [bspc_binary, 'query', '-N', '-d', 'focused', '-n', '.leaf'],
        ]

        try:
            for query in queries:
                result = subprocess.run(query, env=env, capture_output=True, timeout=2)
                if result.returncode != 0:
                    return False
            return True
        except subprocess.TimeoutExpired:
            return False

    # Test command dispatch table
    def test_command_dispatch_variety(self, env, bspc_binary):
        """Test variety of commands that benefit from O(1) dispatch"""
        commands = [
            [bspc_binary, 'query', '-D'],
            [bspc_binary, 'config', 'border_width'],
            [bspc_binary, 'query', '-M'],
            [bspc_binary, 'config', 'window_gap'],
            [bspc_binary, 'query', '-N'],
            [bspc_binary, 'config', 'split_ratio'],
        ]

        try:
            for cmd in commands:
                result = subprocess.run(cmd, env=env, capture_output=True, timeout=1)
                if result.returncode != 0:
                    return False
            return True
        except subprocess.TimeoutExpired:
            return False

    # Test heavy query workload
    def test_heavy_query_workload(self, env, bspc_binary):
        """Simulate heavy bspwm usage with many rapid queries"""
        try:
            # Rapid-fire queries that would stress the old system
            for _ in range(5):  # 5 rapid queries (reduced for reliability)
                queries = [
                    [bspc_binary, 'query', '-T', '-d'],
                    [bspc_binary, 'query', '-N'],
                    [bspc_binary, 'query', '-D'],
                ]
                for query in queries:
                    result = subprocess.run(query, env=env, capture_output=True, timeout=0.5)
                    if result.returncode != 0:
                        return False
            return True
        except subprocess.TimeoutExpired:
            return False

    # Test layout operations that trigger geometry recalculation
    def test_layout_operations_stress(self, env, bspc_binary):
        """Test layout operations that should benefit from geometry caching"""
        operations = [
            [bspc_binary, 'desktop', '-l', 'tiled'],
            [bspc_binary, 'query', '-T', '-d'],  # Query after layout change
            [bspc_binary, 'desktop', '-l', 'monocle'],
            [bspc_binary, 'query', '-T', '-d'],  # Query after layout change
            [bspc_binary, 'desktop', '-l', 'tiled'],   # Back to tiled
            [bspc_binary, 'query', '-T', '-d'],  # Final query
        ]

        try:
            for op in operations:
                result = subprocess.run(op, env=env, capture_output=True, timeout=1)
                if result.returncode != 0:
                    return False
            return True
        except subprocess.TimeoutExpired:
            return False

    @contextmanager
    def isolated_bspwm(self, bspwm_binary, bspc_binary, display_num):
        """Context manager for isolated bspwm instance"""
        display = f":{display_num}"
        xvfb_proc = None
        bspwm_proc = None
        temp_socket = None

        try:
            # Start Xvfb
            print(f"  Starting Xvfb on {display}...")
            xvfb_proc = subprocess.Popen([
                'Xvfb', display,
                '-screen', '0', '1920x1080x24',
                '-ac', '+extension', 'GLX'
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)

            # Create temp socket path
            temp_socket = f"/tmp/bspwm-opt-bench-{display_num}-{os.getpid()}"

            # Set environment
            env = os.environ.copy()
            env['DISPLAY'] = display
            env['BSPWM_SOCKET'] = temp_socket

            # Start bspwm
            print(f"  Starting bspwm ({os.path.basename(bspwm_binary)}) on {display}...")
            bspwm_proc = subprocess.Popen([bspwm_binary],
                                        env=env,
                                        stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
            time.sleep(1)

            # Verify bspwm is responding
            test_proc = subprocess.run([bspc_binary, 'query', '-D'],
                                     env=env, capture_output=True, timeout=5)
            if test_proc.returncode != 0:
                raise RuntimeError(f"bspwm not responding on {display}")

            # Setup test environment with some windows/desktops
            subprocess.run([bspc_binary, 'monitor', '-d', 'I', 'II', 'III', 'IV'],
                         env=env, timeout=2)

            print(f"  ✓ bspwm ready on {display}")
            yield env, bspc_binary

        except Exception as e:
            print(f"  ✗ Failed to setup isolated bspwm: {e}")
            raise
        finally:
            # Cleanup
            for proc in [bspwm_proc, xvfb_proc]:
                if proc:
                    try:
                        proc.terminate()
                        proc.wait(timeout=2)
                    except (subprocess.TimeoutExpired, ProcessLookupError):
                        try:
                            proc.kill()
                        except ProcessLookupError:
                            pass

            if temp_socket and os.path.exists(temp_socket):
                try:
                    os.unlink(temp_socket)
                except OSError:
                    pass

    def run_optimization_benchmarks(self, version_name, bspwm_binary, bspc_binary, display_num):
        """Run benchmarks targeting actual optimizations"""
        print(f"\n=== Optimization Benchmarks: {version_name} ===")

        if not os.path.exists(bspwm_binary) or not os.path.exists(bspc_binary):
            print(f"✗ Binaries not found")
            return {}

        with self.isolated_bspwm(bspwm_binary, bspc_binary, display_num) as (env, bspc):

            print(f"\n  Testing optimizations that should show performance differences:")

            # Geometry cache tests
            print(f"\n  Geometry Cache Performance:")
            self.run_with_stats(f"{version_name}_geometry_burst",
                              self.test_geometry_queries_burst, env, bspc)
            self.run_with_stats(f"{version_name}_geometry_repeated",
                              self.test_repeated_geometry_queries, env, bspc)

            # Query buffer pool tests
            print(f"\n  Query Buffer Pool Performance:")
            self.run_with_stats(f"{version_name}_bulk_queries",
                              self.test_bulk_queries, env, bspc)

            # Command dispatch tests
            print(f"\n  Command Dispatch Performance:")
            self.run_with_stats(f"{version_name}_command_dispatch",
                              self.test_command_dispatch_variety, env, bspc)

            # Heavy workload tests
            print(f"\n  Heavy Workload Performance:")
            self.run_with_stats(f"{version_name}_heavy_queries",
                              self.test_heavy_query_workload, env, bspc)
            self.run_with_stats(f"{version_name}_layout_stress",
                              self.test_layout_operations_stress, env, bspc)

        return self.results

    def compare_optimization_results(self, baseline_name, optimized_name):
        """Compare optimization benchmark results"""
        print(f"\n=== Optimization Performance Comparison ===")
        print(f"Testing areas where {optimized_name} should outperform {baseline_name}")
        print()

        optimizations = [
            ("geometry_burst", "Geometry Cache (burst queries)"),
            ("geometry_repeated", "Geometry Cache (repeated queries)"),
            ("bulk_queries", "Query Buffer Pools"),
            ("command_dispatch", "O(1) Command Dispatch"),
            ("heavy_queries", "Heavy Query Workload"),
            ("layout_stress", "Layout Operations + Caching")
        ]

        improvements = 0
        total_tests = 0

        for test_suffix, description in optimizations:
            baseline_key = f"{baseline_name}_{test_suffix}"
            optimized_key = f"{optimized_name}_{test_suffix}"

            if baseline_key in self.results and optimized_key in self.results:
                baseline = self.results[baseline_key]
                optimized = self.results[optimized_key]

                if baseline['mean'] > 0:
                    speedup = baseline['mean'] / optimized['mean']
                    reduction = (1 - optimized['mean'] / baseline['mean']) * 100

                    print(f"{description}:")
                    print(f"  {baseline_name}: {baseline['mean']:.1f}μs ± {baseline['stdev']:.1f}μs")
                    print(f"  {optimized_name}: {optimized['mean']:.1f}μs ± {optimized['stdev']:.1f}μs")

                    if speedup > 1.1:  # >10% improvement
                        print(f"  🚀 Speedup: {speedup:.2f}x ({reduction:+.1f}%) - SIGNIFICANT IMPROVEMENT")
                        improvements += 1
                    elif speedup > 1.02:  # >2% improvement
                        print(f"  ✅ Speedup: {speedup:.2f}x ({reduction:+.1f}%) - Minor improvement")
                        improvements += 1
                    elif speedup < 0.9:  # >10% slower
                        print(f"  ❌ Regression: {speedup:.2f}x ({reduction:+.1f}%) - SLOWER")
                    else:
                        print(f"  ➡️ Similar: {speedup:.2f}x ({reduction:+.1f}%) - No significant change")

                    total_tests += 1
                    print()

        print(f"=== Summary ===")
        print(f"Optimizations showing improvement: {improvements}/{total_tests}")

        if improvements >= total_tests * 0.7:  # 70%+ improved
            print(f"🎉 {optimized_name} shows clear optimization benefits!")
        elif improvements >= total_tests * 0.5:  # 50%+ improved
            print(f"✅ {optimized_name} shows some optimization benefits")
        else:
            print(f"⚠️ Optimizations not clearly visible in these tests")

    def save_results(self, filename):
        """Save results to JSON"""
        with open(filename, 'w') as f:
            json.dump(self.results, f, indent=2)

if __name__ == "__main__":
    if not shutil.which('Xvfb'):
        print("❌ Xvfb required for optimization benchmarks")
        print("Install with: sudo pacman -S xorg-server-xvfb")
        sys.exit(1)

    bench = OptimizationBenchmark()

    current_binary = "../bspwm"
    current_bspc = "../bspc"
    upstream_binary = "../bspwm-0.9.12"
    upstream_bspc = "../bspc-0.9.12"

    if len(sys.argv) > 1:
        if sys.argv[1] == "current":
            bench.run_optimization_benchmarks("current", current_binary, current_bspc, 97)
            bench.save_results("optimization_bench_current.json")

        elif sys.argv[1] == "upstream":
            bench.run_optimization_benchmarks("upstream", upstream_binary, upstream_bspc, 98)
            bench.save_results("optimization_bench_upstream.json")

        elif sys.argv[1] == "compare":
            try:
                with open("optimization_bench_current.json", 'r') as f:
                    current_results = json.load(f)
                with open("optimization_bench_upstream.json", 'r') as f:
                    upstream_results = json.load(f)

                bench.results.update(current_results)
                bench.results.update(upstream_results)
                bench.compare_optimization_results("upstream", "current")
            except FileNotFoundError:
                print("Run benchmarks first: python3 optimization_bench.py all")

        elif sys.argv[1] == "all":
            print("🎯 Testing Actual bspwm Optimizations")
            print("=" * 50)

            # Test current version
            if os.path.exists(current_binary):
                bench.run_optimization_benchmarks("current", current_binary, current_bspc, 97)
                bench.save_results("optimization_bench_current.json")

            # Test upstream version
            if os.path.exists(upstream_binary):
                bench.run_optimization_benchmarks("upstream", upstream_binary, upstream_bspc, 98)
                bench.save_results("optimization_bench_upstream.json")

            # Compare
            if os.path.exists("optimization_bench_current.json") and os.path.exists("optimization_bench_upstream.json"):
                with open("optimization_bench_current.json", 'r') as f:
                    current_results = json.load(f)
                with open("optimization_bench_upstream.json", 'r') as f:
                    upstream_results = json.load(f)
                bench.results.update(current_results)
                bench.results.update(upstream_results)
                bench.compare_optimization_results("upstream", "current")
    else:
        print("Usage: python3 optimization_bench.py [current|upstream|compare|all]")
        print()
        print("This benchmarks the ACTUAL optimizations in your bspwm:")
        print("- Geometry caching (reduces X11 calls)")
        print("- Query buffer pools (removes malloc/free)")
        print("- O(1) command dispatch table")
        print("- Heavy workload performance")