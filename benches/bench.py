#!/usr/bin/env python3

import subprocess
import time
import statistics
import sys
import os
import json
from pathlib import Path

class BspwmBenchmark:
    def __init__(self):
        self.iterations = 100
        self.warmup_iterations = 10
        self.results = {}

    def run_with_stats(self, name, func, *args):
        """Run benchmark with proper statistical analysis"""
        print(f"Benchmarking {name}...")

        # Warmup
        for _ in range(self.warmup_iterations):
            func(*args)

        # Actual measurements
        times = []
        for i in range(self.iterations):
            start = time.perf_counter_ns()
            result = func(*args)
            end = time.perf_counter_ns()
            times.append(end - start)

            if i % 10 == 0:
                print(f"  {i}/{self.iterations}")

        # Statistical analysis
        times_us = [t / 1000 for t in times]  # Convert to microseconds

        stats = {
            'min': min(times_us),
            'max': max(times_us),
            'mean': statistics.mean(times_us),
            'median': statistics.median(times_us),
            'stdev': statistics.stdev(times_us) if len(times_us) > 1 else 0,
            'p95': sorted(times_us)[int(0.95 * len(times_us))],
            'p99': sorted(times_us)[int(0.99 * len(times_us))],
            'samples': len(times_us)
        }

        self.results[name] = stats

        print(f"  Mean: {stats['mean']:.2f}μs ± {stats['stdev']:.2f}μs")
        print(f"  P95:  {stats['p95']:.2f}μs")
        print(f"  P99:  {stats['p99']:.2f}μs")
        print()

        return stats

    def bspc_query(self, query):
        """Benchmark bspc query commands"""
        try:
            result = subprocess.run(['./bspc', 'query', query],
                                  capture_output=True, text=True,
                                  timeout=1.0)
            return len(result.stdout)
        except:
            return 0

    def window_spawn_cycle(self):
        """Benchmark window spawn/destroy cycle"""
        try:
            # Spawn xterm
            proc = subprocess.Popen(['xterm', '-geometry', '80x24'],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            time.sleep(0.01)  # Let it spawn

            # Kill it
            proc.terminate()
            proc.wait()
            return True
        except:
            return False

    def layout_switch(self):
        """Benchmark layout switching"""
        try:
            subprocess.run(['./bspc', 'desktop', '-l', 'monocle'],
                          capture_output=True, timeout=0.5)
            subprocess.run(['./bspc', 'desktop', '-l', 'tiled'],
                          capture_output=True, timeout=0.5)
            return True
        except:
            return False

    def tree_navigation(self):
        """Benchmark tree navigation commands"""
        commands = ['-n', 'west', '-n', 'east', '-n', 'north', '-n', 'south']
        try:
            for cmd in commands:
                subprocess.run(['./bspc', 'node', cmd],
                              capture_output=True, timeout=0.5)
            return True
        except:
            return False

    def memory_profile(self, binary_path):
        """Get memory usage statistics"""
        try:
            # Start bspwm in background
            proc = subprocess.Popen([binary_path], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            time.sleep(0.5)  # Let it initialize

            # Get memory usage
            status_file = f"/proc/{proc.pid}/status"
            if os.path.exists(status_file):
                with open(status_file, 'r') as f:
                    content = f.read()
                    for line in content.split('\n'):
                        if line.startswith('VmRSS:'):
                            rss_kb = int(line.split()[1])
                            proc.terminate()
                            proc.wait()
                            return rss_kb

            proc.terminate()
            proc.wait()
            return 0
        except:
            return 0

    def run_benchmarks(self, version_name, binary_path):
        """Run full benchmark suite"""
        print(f"=== Benchmarking {version_name} ===\n")

        version_results = {}

        # Query performance
        self.run_with_stats(f"{version_name}_query_nodes", self.bspc_query, '-N')
        self.run_with_stats(f"{version_name}_query_desktops", self.bspc_query, '-D')
        self.run_with_stats(f"{version_name}_query_monitors", self.bspc_query, '-M')

        # Layout operations
        self.run_with_stats(f"{version_name}_layout_switch", self.layout_switch)

        # Tree navigation
        self.run_with_stats(f"{version_name}_tree_nav", self.tree_navigation)

        # Memory usage (single measurement)
        mem_usage = self.memory_profile(binary_path)
        print(f"Memory usage: {mem_usage} KB\n")
        self.results[f"{version_name}_memory"] = {'rss_kb': mem_usage}

        return self.results

    def compare_results(self, baseline_name, optimized_name):
        """Compare two benchmark results"""
        print("=== Performance Comparison ===\n")

        for test_name in self.results:
            if baseline_name in test_name:
                opt_name = test_name.replace(baseline_name, optimized_name)
                if opt_name in self.results:
                    baseline = self.results[test_name]
                    optimized = self.results[opt_name]

                    if 'mean' in baseline and 'mean' in optimized:
                        speedup = baseline['mean'] / optimized['mean']
                        reduction = (1 - optimized['mean'] / baseline['mean']) * 100

                        print(f"{test_name.replace(baseline_name + '_', '')}:")
                        print(f"  Baseline: {baseline['mean']:.2f}μs ± {baseline['stdev']:.2f}μs")
                        print(f"  Optimized: {optimized['mean']:.2f}μs ± {optimized['stdev']:.2f}μs")
                        print(f"  Speedup: {speedup:.2f}x ({reduction:+.1f}%)")

                        # Statistical significance test (basic)
                        if baseline['stdev'] > 0 and optimized['stdev'] > 0:
                            diff = abs(baseline['mean'] - optimized['mean'])
                            combined_std = (baseline['stdev'] + optimized['stdev']) / 2
                            if diff > 2 * combined_std:
                                print(f"  Significance: LIKELY SIGNIFICANT")
                            else:
                                print(f"  Significance: inconclusive")
                        print()

        # Memory comparison
        baseline_mem = self.results.get(f"{baseline_name}_memory")
        optimized_mem = self.results.get(f"{optimized_name}_memory")

        if baseline_mem and optimized_mem:
            b_rss = baseline_mem['rss_kb']
            o_rss = optimized_mem['rss_kb']
            if b_rss > 0:
                mem_reduction = (1 - o_rss / b_rss) * 100
                print(f"Memory usage:")
                print(f"  Baseline: {b_rss} KB")
                print(f"  Optimized: {o_rss} KB")
                print(f"  Change: {mem_reduction:+.1f}%")
                print()

    def save_results(self, filename):
        """Save results to JSON"""
        with open(filename, 'w') as f:
            json.dump(self.results, f, indent=2)

if __name__ == "__main__":
    bench = BspwmBenchmark()

    # Check if we have both versions
    current_binary = "./bspwm"
    original_binary = "./bspwm-0.9.10"  # We'll build this

    if len(sys.argv) > 1:
        if sys.argv[1] == "current":
            bench.run_benchmarks("current", current_binary)
            bench.save_results("bench_current.json")
        elif sys.argv[1] == "original":
            bench.run_benchmarks("original", original_binary)
            bench.save_results("bench_original.json")
        elif sys.argv[1] == "compare":
            # Load previous results and compare
            try:
                with open("bench_current.json", 'r') as f:
                    current_results = json.load(f)
                with open("bench_original.json", 'r') as f:
                    original_results = json.load(f)

                bench.results.update(current_results)
                bench.results.update(original_results)
                bench.compare_results("original", "current")
            except FileNotFoundError:
                print("Run 'python3 bench.py current' and 'python3 bench.py original' first")
    else:
        print("Usage: python3 bench.py [current|original|compare]")