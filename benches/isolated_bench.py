#!/usr/bin/env python3

import subprocess
import time
import statistics
import sys
import os
import json
import signal
import tempfile
import shutil
from pathlib import Path
from contextlib import contextmanager

class IsolatedBspwmBenchmark:
    def __init__(self):
        self.iterations = 50  # Reduced for isolation overhead
        self.warmup_iterations = 5
        self.results = {}
        self.cleanup_pids = []

    def __del__(self):
        self.cleanup_all()

    def cleanup_all(self):
        """Clean up any remaining processes"""
        for pid in self.cleanup_pids:
            try:
                os.kill(pid, signal.SIGTERM)
                time.sleep(0.1)
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        self.cleanup_pids.clear()

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
            self.cleanup_pids.append(xvfb_proc.pid)
            time.sleep(1)  # Let Xvfb start

            # Create temp socket path
            temp_socket = f"/tmp/bspwm-bench-{display_num}-{os.getpid()}"

            # Set environment for this bspwm instance
            env = os.environ.copy()
            env['DISPLAY'] = display
            env['BSPWM_SOCKET'] = temp_socket

            # Start bspwm
            print(f"  Starting bspwm ({os.path.basename(bspwm_binary)}) on {display}...")
            bspwm_proc = subprocess.Popen([bspwm_binary],
                                        env=env,
                                        stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
            self.cleanup_pids.append(bspwm_proc.pid)
            time.sleep(1)  # Let bspwm initialize

            # Verify bspwm is responding
            test_proc = subprocess.run([bspc_binary, 'query', '-D'],
                                     env=env, capture_output=True, timeout=5)
            if test_proc.returncode != 0:
                raise RuntimeError(f"bspwm not responding on {display}")

            print(f"  ✓ bspwm ready on {display}")

            # Yield the environment for testing
            yield env, bspc_binary

        except Exception as e:
            print(f"  ✗ Failed to setup isolated bspwm: {e}")
            raise
        finally:
            # Cleanup
            if bspwm_proc:
                try:
                    bspwm_proc.terminate()
                    bspwm_proc.wait(timeout=2)
                except (subprocess.TimeoutExpired, ProcessLookupError):
                    try:
                        bspwm_proc.kill()
                    except ProcessLookupError:
                        pass
                if bspwm_proc.pid in self.cleanup_pids:
                    self.cleanup_pids.remove(bspwm_proc.pid)

            if xvfb_proc:
                try:
                    xvfb_proc.terminate()
                    xvfb_proc.wait(timeout=2)
                except (subprocess.TimeoutExpired, ProcessLookupError):
                    try:
                        xvfb_proc.kill()
                    except ProcessLookupError:
                        pass
                if xvfb_proc.pid in self.cleanup_pids:
                    self.cleanup_pids.remove(xvfb_proc.pid)

            # Clean up socket
            if temp_socket and os.path.exists(temp_socket):
                try:
                    os.unlink(temp_socket)
                except OSError:
                    pass

    def setup_test_scenario(self, env, bspc_binary, scenario="default"):
        """Setup a specific window layout scenario"""
        try:
            if scenario == "default":
                # Create a basic desktop layout
                subprocess.run([bspc_binary, 'monitor', '-d', 'I', 'II', 'III'],
                             env=env, check=True, timeout=2)

            elif scenario == "complex":
                # Create complex layout with nested splits
                subprocess.run([bspc_binary, 'monitor', '-d', 'test'],
                             env=env, check=True, timeout=2)

                # Simulate windows by creating nodes (without actual X windows)
                for i in range(10):
                    # We can't easily create fake windows, but we can test the commands
                    pass

        except subprocess.CalledProcessError as e:
            print(f"  Warning: Failed to setup scenario '{scenario}': {e}")

    def run_with_stats(self, name, test_func, env, bspc_binary):
        """Run benchmark with proper statistical analysis"""
        print(f"    Benchmarking {name}...")

        # Warmup
        for _ in range(self.warmup_iterations):
            try:
                test_func(env, bspc_binary)
            except:
                pass  # Ignore warmup failures

        # Actual measurements
        times = []
        successful_runs = 0

        for i in range(self.iterations):
            try:
                start = time.perf_counter_ns()
                result = test_func(env, bspc_binary)
                end = time.perf_counter_ns()

                if result is not False:  # Allow None, but not False (explicit failure)
                    times.append(end - start)
                    successful_runs += 1

            except Exception as e:
                # Skip failed iterations but track them
                pass

        if not times:
            print(f"    ✗ All benchmark iterations failed for {name}")
            return None

        if successful_runs < self.iterations * 0.8:  # Less than 80% success rate
            print(f"    ⚠ Low success rate for {name}: {successful_runs}/{self.iterations}")

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

        print(f"    Mean: {stats['mean']:.2f}μs ± {stats['stdev']:.2f}μs "
              f"(success: {stats['success_rate']:.1%})")

        return stats

    # Test functions that work with bspc
    def test_query_desktops(self, env, bspc_binary):
        """Test desktop query performance"""
        result = subprocess.run([bspc_binary, 'query', '-D'],
                              env=env, capture_output=True, timeout=1)
        return len(result.stdout) if result.returncode == 0 else False

    def test_query_monitors(self, env, bspc_binary):
        """Test monitor query performance"""
        result = subprocess.run([bspc_binary, 'query', '-M'],
                              env=env, capture_output=True, timeout=1)
        return len(result.stdout) if result.returncode == 0 else False

    def test_query_nodes(self, env, bspc_binary):
        """Test node query performance"""
        result = subprocess.run([bspc_binary, 'query', '-N'],
                              env=env, capture_output=True, timeout=1)
        return len(result.stdout) if result.returncode == 0 else False

    def test_desktop_operations(self, env, bspc_binary):
        """Test desktop switching operations"""
        try:
            # Switch to desktop II
            subprocess.run([bspc_binary, 'desktop', '-f', 'II'],
                          env=env, check=True, timeout=1)
            # Switch back to I
            subprocess.run([bspc_binary, 'desktop', '-f', 'I'],
                          env=env, check=True, timeout=1)
            return True
        except subprocess.CalledProcessError:
            return False

    def test_layout_operations(self, env, bspc_binary):
        """Test layout switching"""
        try:
            subprocess.run([bspc_binary, 'desktop', '-l', 'monocle'],
                          env=env, check=True, timeout=1)
            subprocess.run([bspc_binary, 'desktop', '-l', 'tiled'],
                          env=env, check=True, timeout=1)
            return True
        except subprocess.CalledProcessError:
            return False

    def test_config_operations(self, env, bspc_binary):
        """Test configuration queries"""
        result = subprocess.run([bspc_binary, 'config', 'border_width'],
                              env=env, capture_output=True, timeout=1)
        return len(result.stdout) if result.returncode == 0 else False

    def get_memory_usage(self, bspwm_proc):
        """Get memory usage of bspwm process"""
        try:
            result = subprocess.run(['ps', '-p', str(bspwm_proc.pid), '-o', 'rss='],
                                  capture_output=True, text=True, timeout=1)
            if result.returncode == 0:
                return int(result.stdout.strip())  # RSS in KB
        except:
            pass
        return 0

    def run_benchmarks(self, version_name, bspwm_binary, bspc_binary, display_num):
        """Run full benchmark suite for a version"""
        print(f"\n=== Benchmarking {version_name} ===")

        if not os.path.exists(bspwm_binary):
            print(f"✗ Binary not found: {bspwm_binary}")
            return {}

        if not os.path.exists(bspc_binary):
            print(f"✗ Binary not found: {bspc_binary}")
            return {}

        # Verify version
        try:
            result = subprocess.run([bspwm_binary, '--version'],
                                  capture_output=True, text=True, timeout=2)
            actual_version = result.stdout.strip() if result.returncode == 0 else "unknown"
            print(f"Version: {actual_version}")
        except:
            print("Version: unknown")

        version_results = {}

        with self.isolated_bspwm(bspwm_binary, bspc_binary, display_num) as (env, bspc):
            # Setup test scenario
            self.setup_test_scenario(env, bspc, "default")

            # Run benchmarks
            tests = [
                (f"{version_name}_query_desktops", self.test_query_desktops),
                (f"{version_name}_query_monitors", self.test_query_monitors),
                (f"{version_name}_query_nodes", self.test_query_nodes),
                (f"{version_name}_desktop_ops", self.test_desktop_operations),
                (f"{version_name}_layout_ops", self.test_layout_operations),
                (f"{version_name}_config_ops", self.test_config_operations),
            ]

            for test_name, test_func in tests:
                self.run_with_stats(test_name, test_func, env, bspc)

        return version_results

    def compare_results(self, baseline_name, optimized_name):
        """Compare benchmark results between versions"""
        print(f"\n=== Performance Comparison: {baseline_name} vs {optimized_name} ===\n")

        comparisons = []
        for test_name in self.results:
            if baseline_name in test_name:
                opt_name = test_name.replace(baseline_name, optimized_name)
                if opt_name in self.results:
                    baseline = self.results[test_name]
                    optimized = self.results[opt_name]

                    if 'mean' in baseline and 'mean' in optimized and baseline['mean'] > 0:
                        speedup = baseline['mean'] / optimized['mean']
                        reduction = (1 - optimized['mean'] / baseline['mean']) * 100

                        test_display = test_name.replace(f"{baseline_name}_", "")

                        print(f"{test_display}:")
                        print(f"  {baseline_name}: {baseline['mean']:.2f}μs ± {baseline['stdev']:.2f}μs")
                        print(f"  {optimized_name}: {optimized['mean']:.2f}μs ± {optimized['stdev']:.2f}μs")
                        print(f"  Speedup: {speedup:.2f}x ({reduction:+.1f}%)")

                        # Simple significance test
                        if baseline['stdev'] > 0 and optimized['stdev'] > 0:
                            diff = abs(baseline['mean'] - optimized['mean'])
                            combined_std = (baseline['stdev'] + optimized['stdev']) / 2
                            if diff > 2 * combined_std:
                                print(f"  Significance: LIKELY SIGNIFICANT")
                            else:
                                print(f"  Significance: inconclusive")
                        print()

                        comparisons.append({
                            'test': test_display,
                            'speedup': speedup,
                            'reduction': reduction
                        })

        # Summary
        if comparisons:
            avg_speedup = statistics.mean(c['speedup'] for c in comparisons)
            print(f"Average speedup: {avg_speedup:.2f}x")

            improvements = [c for c in comparisons if c['speedup'] > 1.05]  # >5% improvement
            regressions = [c for c in comparisons if c['speedup'] < 0.95]   # >5% regression

            print(f"Improvements: {len(improvements)}/{len(comparisons)}")
            print(f"Regressions: {len(regressions)}/{len(comparisons)}")

    def save_results(self, filename):
        """Save results to JSON"""
        with open(filename, 'w') as f:
            json.dump(self.results, f, indent=2)

def check_dependencies():
    """Check if required tools are available"""
    required = ['Xvfb', 'ps']
    missing = []

    for tool in required:
        if not shutil.which(tool):
            missing.append(tool)

    if missing:
        print(f"Missing required tools: {', '.join(missing)}")
        if 'Xvfb' in missing:
            print("Install Xvfb with: sudo pacman -S xorg-server-xvfb")
        return False

    return True

if __name__ == "__main__":
    if not check_dependencies():
        sys.exit(1)

    bench = IsolatedBspwmBenchmark()

    # Auto-detect binaries
    current_binary = "../bspwm"
    current_bspc = "../bspc"
    upstream_binary = "../bspwm-0.9.12"
    upstream_bspc = "../bspc-0.9.12"

    if len(sys.argv) > 1:
        if sys.argv[1] == "current":
            bench.run_benchmarks("current", current_binary, current_bspc, 99)
            bench.save_results("isolated_bench_current.json")

        elif sys.argv[1] == "upstream":
            bench.run_benchmarks("upstream", upstream_binary, upstream_bspc, 100)
            bench.save_results("isolated_bench_upstream.json")

        elif sys.argv[1] == "compare":
            try:
                with open("isolated_bench_current.json", 'r') as f:
                    current_results = json.load(f)
                with open("isolated_bench_upstream.json", 'r') as f:
                    upstream_results = json.load(f)

                bench.results.update(current_results)
                bench.results.update(upstream_results)
                bench.compare_results("upstream", "current")
            except FileNotFoundError as e:
                print(f"Error: {e}")
                print("Run 'python3 isolated_bench.py current' and 'python3 isolated_bench.py upstream' first")
                sys.exit(1)

        elif sys.argv[1] == "all":
            print("=== Running Complete Isolated Benchmark Suite ===")

            current_results = {}
            upstream_results = {}

            # Current version
            if os.path.exists(current_binary) and os.path.exists(current_bspc):
                bench.results.clear()  # Clear before running
                bench.run_benchmarks("current", current_binary, current_bspc, 99)
                current_results = bench.results.copy()
                bench.save_results("isolated_bench_current.json")
            else:
                print(f"Warning: Current binaries not found, skipping")

            # Upstream version
            if os.path.exists(upstream_binary) and os.path.exists(upstream_bspc):
                bench.results.clear()  # Clear before running
                bench.run_benchmarks("upstream", upstream_binary, upstream_bspc, 100)
                upstream_results = bench.results.copy()
                bench.save_results("isolated_bench_upstream.json")
            else:
                print(f"Warning: Upstream binaries not found, skipping")

            # Compare
            if current_results and upstream_results:
                bench.results.clear()
                bench.results.update(current_results)
                bench.results.update(upstream_results)
                bench.compare_results("upstream", "current")
        else:
            print(f"Unknown command: {sys.argv[1]}")
            sys.exit(1)
    else:
        print("Usage: python3 isolated_bench.py [current|upstream|compare|all]")
        print("")
        print("This script runs bspwm versions in isolated Xvfb environments")
        print("to ensure accurate performance comparisons.")
        print("")
        print("Commands:")
        print("  current   - Benchmark current bspwm version")
        print("  upstream  - Benchmark upstream bspwm version")
        print("  compare   - Compare previous benchmark results")
        print("  all       - Run complete isolated benchmark suite")