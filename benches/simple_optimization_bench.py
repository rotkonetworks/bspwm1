#!/usr/bin/env python3

import subprocess
import time
import statistics
import sys
import os
import json
import shutil
from contextlib import contextmanager

class SimpleOptimizationBenchmark:
    """Simple, robust benchmark for bspwm optimizations"""

    def __init__(self):
        self.iterations = 50
        self.warmup_iterations = 5
        self.results = {}

    def run_with_stats(self, name, test_func, env, bspc_binary):
        """Run benchmark with proper statistical analysis"""
        print(f"    {name}...")

        # Warmup
        for _ in range(self.warmup_iterations):
            try:
                test_func(env, bspc_binary)
            except:
                pass

        times = []
        for i in range(self.iterations):
            try:
                start = time.perf_counter_ns()
                success = test_func(env, bspc_binary)
                end = time.perf_counter_ns()

                if success:
                    times.append((end - start) / 1000)  # Convert to microseconds
            except Exception:
                pass

        if len(times) < self.iterations * 0.8:  # Need 80% success rate
            print(f"      ❌ Failed (only {len(times)}/{self.iterations} successful)")
            return None

        stats = {
            'mean': statistics.mean(times),
            'stdev': statistics.stdev(times) if len(times) > 1 else 0,
            'min': min(times),
            'max': max(times),
            'samples': len(times)
        }

        self.results[name] = stats
        print(f"      ✅ {stats['mean']:.1f}μs ± {stats['stdev']:.1f}μs")
        return stats

    # Simple, robust tests
    def test_basic_queries(self, env, bspc_binary):
        """Test basic query performance"""
        commands = [
            [bspc_binary, 'query', '-D'],  # Desktop queries
            [bspc_binary, 'query', '-M'],  # Monitor queries
        ]
        try:
            for cmd in commands:
                result = subprocess.run(cmd, env=env, capture_output=True, timeout=2)
                if result.returncode != 0:
                    return False
            return True
        except:
            return False

    def test_repeated_queries(self, env, bspc_binary):
        """Test repeated queries (should benefit from caching)"""
        try:
            # Query the same thing 3 times rapidly
            for _ in range(3):
                result = subprocess.run([bspc_binary, 'query', '-D'],
                                      env=env, capture_output=True, timeout=2)
                if result.returncode != 0:
                    return False
            return True
        except:
            return False

    def test_config_queries(self, env, bspc_binary):
        """Test config queries (command dispatch)"""
        configs = ['border_width', 'window_gap', 'split_ratio']
        try:
            for config in configs:
                result = subprocess.run([bspc_binary, 'config', config],
                                      env=env, capture_output=True, timeout=2)
                if result.returncode != 0:
                    return False
            return True
        except:
            return False

    def test_mixed_workload(self, env, bspc_binary):
        """Test mixed query/config workload"""
        commands = [
            [bspc_binary, 'query', '-D'],
            [bspc_binary, 'config', 'border_width'],
            [bspc_binary, 'query', '-M'],
            [bspc_binary, 'config', 'window_gap'],
        ]
        try:
            for cmd in commands:
                result = subprocess.run(cmd, env=env, capture_output=True, timeout=2)
                if result.returncode != 0:
                    return False
            return True
        except:
            return False

    @contextmanager
    def isolated_bspwm(self, bspwm_binary, bspc_binary, display_num):
        """Start isolated bspwm instance"""
        display = f":{display_num}"
        xvfb_proc = None
        bspwm_proc = None

        try:
            print(f"  Starting isolated bspwm on {display}...")

            # Start Xvfb
            xvfb_proc = subprocess.Popen([
                'Xvfb', display, '-screen', '0', '1920x1080x24', '-ac'
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)

            # Start bspwm
            env = os.environ.copy()
            env['DISPLAY'] = display
            env['BSPWM_SOCKET'] = f"/tmp/bspwm-simple-{display_num}-{os.getpid()}"

            bspwm_proc = subprocess.Popen([bspwm_binary], env=env,
                                        stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
            time.sleep(1)

            # Test basic connectivity
            result = subprocess.run([bspc_binary, 'query', '-D'],
                                  env=env, capture_output=True, timeout=3)
            if result.returncode != 0:
                raise RuntimeError("bspwm not responding")

            print(f"  ✅ Ready")
            yield env, bspc_binary

        finally:
            for proc in [bspwm_proc, xvfb_proc]:
                if proc:
                    try:
                        proc.terminate()
                        proc.wait(timeout=1)
                    except:
                        try:
                            proc.kill()
                        except:
                            pass

    def run_benchmarks(self, version_name, bspwm_binary, bspc_binary, display_num):
        """Run benchmarks for a version"""
        print(f"\n=== {version_name.upper()} VERSION ===")

        with self.isolated_bspwm(bspwm_binary, bspc_binary, display_num) as (env, bspc):
            tests = [
                ("basic_queries", self.test_basic_queries),
                ("repeated_queries", self.test_repeated_queries),
                ("config_queries", self.test_config_queries),
                ("mixed_workload", self.test_mixed_workload),
            ]

            for test_name, test_func in tests:
                full_name = f"{version_name}_{test_name}"
                self.run_with_stats(full_name, test_func, env, bspc)

    def compare_results(self, baseline, optimized):
        """Compare results between versions"""
        print(f"\n=== PERFORMANCE COMPARISON ===")
        print(f"{optimized.upper()} vs {baseline.upper()}")
        print("-" * 50)

        improvements = 0
        total_tests = 0

        for test_name in ["basic_queries", "repeated_queries", "config_queries", "mixed_workload"]:
            baseline_key = f"{baseline}_{test_name}"
            optimized_key = f"{optimized}_{test_name}"

            if baseline_key in self.results and optimized_key in self.results:
                b_mean = self.results[baseline_key]['mean']
                o_mean = self.results[optimized_key]['mean']

                speedup = b_mean / o_mean
                improvement = (1 - o_mean / b_mean) * 100

                print(f"{test_name.replace('_', ' ').title()}:")
                print(f"  {baseline}: {b_mean:.1f}μs")
                print(f"  {optimized}: {o_mean:.1f}μs")

                if speedup > 1.1:
                    print(f"  🚀 {speedup:.2f}x faster ({improvement:+.1f}%) - SIGNIFICANT")
                    improvements += 1
                elif speedup > 1.02:
                    print(f"  ✅ {speedup:.2f}x faster ({improvement:+.1f}%) - Minor improvement")
                    improvements += 1
                elif speedup < 0.9:
                    print(f"  ❌ {speedup:.2f}x slower ({improvement:+.1f}%) - REGRESSION")
                else:
                    print(f"  ➡️  {speedup:.2f}x ({improvement:+.1f}%) - Similar performance")

                total_tests += 1
                print()

        print(f"SUMMARY: {improvements}/{total_tests} tests show improvement")

        if improvements >= total_tests * 0.75:
            print("🎉 Clear optimization benefits!")
        elif improvements >= total_tests * 0.5:
            print("✅ Some optimization benefits visible")
        else:
            print("⚠️  Limited optimization benefits in these tests")

    def save_results(self, filename):
        """Save results to JSON"""
        with open(filename, 'w') as f:
            json.dump(self.results, f, indent=2)

if __name__ == "__main__":
    if not shutil.which('Xvfb'):
        print("❌ Xvfb required. Install: sudo pacman -S xorg-server-xvfb")
        sys.exit(1)

    bench = SimpleOptimizationBenchmark()

    current_binary = "../bspwm"
    current_bspc = "../bspc"
    upstream_binary = "../bspwm-0.9.12"
    upstream_bspc = "../bspc-0.9.12"

    if len(sys.argv) > 1 and sys.argv[1] == "all":
        print("🎯 SIMPLE OPTIMIZATION BENCHMARK")
        print("Testing the actual optimizations where they should matter")
        print("=" * 60)

        # Test current version
        if os.path.exists(current_binary) and os.path.exists(current_bspc):
            bench.run_benchmarks("current", current_binary, current_bspc, 95)

        # Test upstream version
        if os.path.exists(upstream_binary) and os.path.exists(upstream_bspc):
            bench.run_benchmarks("upstream", upstream_binary, upstream_bspc, 96)

        # Compare
        if len(bench.results) >= 8:  # We expect 4 tests × 2 versions = 8 results
            bench.compare_results("upstream", "current")

        print("\n📊 Results saved to simple_optimization_results.json")
        bench.save_results("simple_optimization_results.json")

    else:
        print("Usage: python3 simple_optimization_bench.py all")
        print()
        print("This tests bspwm optimizations in areas where they should matter:")
        print("- Query performance (geometry cache, buffer pools)")
        print("- Command dispatch (O(1) vs linear lookup)")
        print("- Mixed workloads (real-world usage patterns)")