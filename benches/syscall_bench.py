#!/usr/bin/env python3

import subprocess
import re
import json
import sys
import os
from pathlib import Path

def parse_strace_output(output):
    """Parse strace output to extract syscall statistics"""
    stats = {}

    # Look for the summary lines at the end
    lines = output.split('\n')
    summary_started = False

    for line in lines:
        if '% time     seconds  usecs/call     calls    errors syscall' in line:
            summary_started = True
            continue

        if summary_started and line.strip():
            # Parse line like: " 25.64      0.000051          3        17           write"
            parts = line.split()
            if len(parts) >= 6 and parts[0].replace('.', '').isdigit():
                try:
                    pct_time = float(parts[0])
                    total_seconds = float(parts[1])
                    usecs_per_call = int(parts[2]) if parts[2] != '?' else 0
                    calls = int(parts[3])
                    errors = int(parts[4]) if parts[4] != '?' else 0
                    syscall = parts[5]

                    stats[syscall] = {
                        'percent_time': pct_time,
                        'total_seconds': total_seconds,
                        'usecs_per_call': usecs_per_call,
                        'calls': calls,
                        'errors': errors
                    }
                except (ValueError, IndexError):
                    continue

    return stats

def benchmark_binary(binary_path, name):
    """Benchmark a binary using strace"""
    print(f"Analyzing {name}...")

    # Run with strace to analyze syscalls
    cmd = ['strace', '-c', '-f', '-o', '/dev/stdout', binary_path, '--version']

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        strace_output = result.stderr  # strace outputs to stderr

        stats = parse_strace_output(strace_output)

        if not stats:
            print(f"Warning: No syscall stats found for {name}")
            return {}

        # Focus on performance-critical syscalls
        important_syscalls = [
            'write', 'read', 'poll', 'select', 'epoll_wait',
            'connect', 'accept', 'recv', 'send', 'sendto', 'recvfrom',
            'mmap', 'munmap', 'brk', 'malloc', 'free',
            'open', 'openat', 'close', 'stat', 'lstat', 'fstat',
            'clone', 'execve', 'wait4', 'waitpid'
        ]

        critical_stats = {}
        total_calls = sum(s['calls'] for s in stats.values())
        total_time = sum(s['total_seconds'] for s in stats.values())

        print(f"  Total syscalls: {total_calls}")
        print(f"  Total time: {total_time:.6f}s")

        for syscall in important_syscalls:
            if syscall in stats:
                s = stats[syscall]
                critical_stats[syscall] = s
                print(f"  {syscall:15}: {s['calls']:6d} calls, {s['usecs_per_call']:4d}μs/call")

        return {
            'total_calls': total_calls,
            'total_time': total_time,
            'syscalls': critical_stats
        }

    except subprocess.TimeoutExpired:
        print(f"Timeout running {name}")
        return {}
    except Exception as e:
        print(f"Error analyzing {name}: {e}")
        return {}

def analyze_binary_size(binary_path):
    """Analyze binary size and symbols"""
    try:
        # Get binary size
        size_result = subprocess.run(['size', binary_path], capture_output=True, text=True)
        size_lines = size_result.stdout.strip().split('\n')

        if len(size_lines) >= 2:
            # Parse size output: text    data     bss     dec     hex filename
            parts = size_lines[1].split()
            if len(parts) >= 4:
                text_size = int(parts[0])
                data_size = int(parts[1])
                bss_size = int(parts[2])
                total_size = int(parts[3])

                print(f"Binary size analysis:")
                print(f"  Text (code): {text_size:,} bytes")
                print(f"  Data:        {data_size:,} bytes")
                print(f"  BSS:         {bss_size:,} bytes")
                print(f"  Total:       {total_size:,} bytes")

                return {
                    'text_size': text_size,
                    'data_size': data_size,
                    'bss_size': bss_size,
                    'total_size': total_size
                }
    except Exception as e:
        print(f"Error analyzing binary size: {e}")

    return {}

def compare_results(baseline, optimized, baseline_name, optimized_name):
    """Compare benchmark results"""
    print("\n" + "="*60)
    print("PERFORMANCE COMPARISON")
    print("="*60)

    if not baseline or not optimized:
        print("Missing benchmark data for comparison")
        return

    # Compare total syscalls
    if 'total_calls' in baseline and 'total_calls' in optimized:
        baseline_calls = baseline['total_calls']
        optimized_calls = optimized['total_calls']

        if baseline_calls > 0:
            reduction = (baseline_calls - optimized_calls) / baseline_calls * 100
            print(f"\nTotal syscalls:")
            print(f"  {baseline_name:12}: {baseline_calls:,}")
            print(f"  {optimized_name:12}: {optimized_calls:,}")
            print(f"  Reduction:     {reduction:+.1f}%")

    # Compare total time
    if 'total_time' in baseline and 'total_time' in optimized:
        baseline_time = baseline['total_time']
        optimized_time = optimized['total_time']

        if baseline_time > 0:
            speedup = baseline_time / optimized_time if optimized_time > 0 else float('inf')
            reduction = (baseline_time - optimized_time) / baseline_time * 100
            print(f"\nTotal syscall time:")
            print(f"  {baseline_name:12}: {baseline_time:.6f}s")
            print(f"  {optimized_name:12}: {optimized_time:.6f}s")
            print(f"  Speedup:       {speedup:.2f}x ({reduction:+.1f}%)")

    # Compare individual syscalls
    if 'syscalls' in baseline and 'syscalls' in optimized:
        print(f"\nPer-syscall comparison:")
        print(f"{'Syscall':<15} {'Baseline':<12} {'Optimized':<12} {'Change':<10}")
        print("-" * 55)

        all_syscalls = set(baseline['syscalls'].keys()) | set(optimized['syscalls'].keys())

        for syscall in sorted(all_syscalls):
            baseline_calls = baseline['syscalls'].get(syscall, {}).get('calls', 0)
            optimized_calls = optimized['syscalls'].get(syscall, {}).get('calls', 0)

            if baseline_calls > 0 or optimized_calls > 0:
                if baseline_calls > 0:
                    change = (optimized_calls - baseline_calls) / baseline_calls * 100
                    print(f"{syscall:<15} {baseline_calls:<12,} {optimized_calls:<12,} {change:+6.1f}%")
                else:
                    print(f"{syscall:<15} {baseline_calls:<12,} {optimized_calls:<12,} {'NEW':>9}")

def main():
    current_binary = "./bspwm"
    original_binary = "./bspwm-0.9.10"

    if not os.path.exists(current_binary):
        print(f"Current binary not found: {current_binary}")
        sys.exit(1)

    if not os.path.exists(original_binary):
        print(f"Original binary not found: {original_binary}")
        sys.exit(1)

    print("BSPWM SYSCALL ANALYSIS")
    print("="*50)

    # Analyze both binaries
    print(f"\nAnalyzing {current_binary}...")
    current_stats = benchmark_binary(current_binary, "current")
    current_size = analyze_binary_size(current_binary)

    print(f"\nAnalyzing {original_binary}...")
    original_stats = benchmark_binary(original_binary, "original")
    original_size = analyze_binary_size(original_binary)

    # Compare binary sizes
    if current_size and original_size:
        print(f"\nBinary size comparison:")
        current_total = current_size['total_size']
        original_total = original_size['total_size']
        size_change = (current_total - original_total) / original_total * 100
        print(f"  Original: {original_total:,} bytes")
        print(f"  Current:  {current_total:,} bytes")
        print(f"  Change:   {size_change:+.1f}%")

    # Compare performance
    compare_results(original_stats, current_stats, "original", "current")

    # Save detailed results
    results = {
        'current': {
            'stats': current_stats,
            'size': current_size
        },
        'original': {
            'stats': original_stats,
            'size': original_size
        }
    }

    with open('syscall_analysis.json', 'w') as f:
        json.dump(results, f, indent=2)

    print(f"\nDetailed results saved to syscall_analysis.json")

if __name__ == "__main__":
    main()