#!/usr/bin/env python3

import subprocess
import time
import statistics
import sys
import os
import tempfile
import json
from pathlib import Path

class BspcBenchmark:
    def __init__(self, bspwm_binary, bspc_binary):
        self.bspwm_binary = bspwm_binary
        self.bspc_binary = bspc_binary
        self.bspwm_process = None
        self.socket_path = None

    def start_bspwm(self):
        """Start bspwm in background with temp socket"""
        # Create temporary socket
        self.socket_path = tempfile.mktemp(prefix='bspwm_bench_')

        env = os.environ.copy()
        env['BSPWM_SOCKET'] = self.socket_path

        # Start bspwm
        self.bspwm_process = subprocess.Popen(
            [self.bspwm_binary],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )

        # Wait for socket to be created
        for _ in range(50):  # 5 second timeout
            if os.path.exists(self.socket_path):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("bspwm failed to create socket")

        time.sleep(0.1)  # Let it fully initialize

    def stop_bspwm(self):
        """Stop bspwm and clean up"""
        if self.bspwm_process:
            self.bspwm_process.terminate()
            self.bspwm_process.wait()
            self.bspwm_process = None

        if self.socket_path and os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
            self.socket_path = None

    def bspc_command(self, *args):
        """Execute bspc command and return timing"""
        env = os.environ.copy()
        env['BSPWM_SOCKET'] = self.socket_path

        start = time.perf_counter_ns()
        try:
            result = subprocess.run(
                [self.bspc_binary] + list(args),
                env=env,
                capture_output=True,
                text=True,
                timeout=2.0
            )
            end = time.perf_counter_ns()

            # Return timing in microseconds
            return (end - start) / 1000, result.returncode == 0
        except subprocess.TimeoutExpired:
            return 2000000, False  # 2 second timeout

    def benchmark_commands(self, iterations=1000):
        """Benchmark various bspc commands"""
        commands = [
            ('query_nodes', ['query', '-N']),
            ('query_desktops', ['query', '-D']),
            ('query_monitors', ['query', '-M']),
            ('query_tree', ['query', '-T']),
            ('config_get', ['config', 'border_width']),
            ('wm_dump', ['wm', '--dump-state']),
        ]

        results = {}

        for name, cmd in commands:
            print(f"Benchmarking {name}...")

            times = []
            success_count = 0

            # Warmup
            for _ in range(10):
                self.bspc_command(*cmd)

            # Actual measurement
            for i in range(iterations):
                duration_us, success = self.bspc_command(*cmd)
                times.append(duration_us)
                if success:
                    success_count += 1

                if i % 100 == 0:
                    print(f"  {i}/{iterations}")

            # Calculate statistics
            if times:
                stats = {
                    'min': min(times),
                    'max': max(times),
                    'mean': statistics.mean(times),
                    'median': statistics.median(times),
                    'stdev': statistics.stdev(times) if len(times) > 1 else 0,
                    'p95': sorted(times)[int(0.95 * len(times))],
                    'p99': sorted(times)[int(0.99 * len(times))],
                    'success_rate': success_count / iterations,
                    'samples': len(times)
                }

                results[name] = stats

                print(f"  Mean: {stats['mean']:.1f}μs ± {stats['stdev']:.1f}μs")
                print(f"  P95:  {stats['p95']:.1f}μs")
                print(f"  Success: {stats['success_rate']*100:.1f}%")
                print()

        return results

    def memory_usage(self):
        """Get current memory usage of bspwm process"""
        if not self.bspwm_process:
            return 0

        try:
            with open(f"/proc/{self.bspwm_process.pid}/status", 'r') as f:
                content = f.read()
                for line in content.split('\n'):
                    if line.startswith('VmRSS:'):
                        return int(line.split()[1])
            return 0
        except:
            return 0

def main():
    if len(sys.argv) != 3:
        print("Usage: python3 bspc_bench.py <bspwm_binary> <bspc_binary>")
        sys.exit(1)

    bspwm_binary = sys.argv[1]
    bspc_binary = sys.argv[2]

    if not os.path.exists(bspwm_binary):
        print(f"bspwm binary not found: {bspwm_binary}")
        sys.exit(1)

    if not os.path.exists(bspc_binary):
        print(f"bspc binary not found: {bspc_binary}")
        sys.exit(1)

    print(f"Benchmarking: {bspwm_binary} with {bspc_binary}")
    print("=" * 50)

    bench = BspcBenchmark(bspwm_binary, bspc_binary)

    try:
        bench.start_bspwm()
        print(f"bspwm started (PID: {bench.bspwm_process.pid})")

        # Get initial memory usage
        initial_memory = bench.memory_usage()
        print(f"Initial memory usage: {initial_memory} KB\n")

        # Run benchmarks
        results = bench.benchmark_commands(iterations=500)

        # Get final memory usage
        final_memory = bench.memory_usage()
        print(f"Final memory usage: {final_memory} KB")
        print(f"Memory delta: {final_memory - initial_memory:+d} KB\n")

        # Save results
        output_file = f"bench_results_{Path(bspwm_binary).name}.json"
        with open(output_file, 'w') as f:
            json.dump({
                'binary': bspwm_binary,
                'initial_memory': initial_memory,
                'final_memory': final_memory,
                'results': results
            }, f, indent=2)

        print(f"Results saved to {output_file}")

    finally:
        bench.stop_bspwm()

if __name__ == "__main__":
    main()