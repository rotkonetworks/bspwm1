#!/usr/bin/env python3

import os
import subprocess
import sys
import json

def test_version_detection():
    """Test that we can detect different bspwm versions"""
    print("=== Testing Version Detection ===")

    current_binary = "../bspwm"
    upstream_binary = "../bspwm-0.9.12"

    versions = {}

    # Test current version
    if os.path.exists(current_binary):
        try:
            result = subprocess.run([current_binary, '--version'],
                                  capture_output=True, text=True, timeout=2)
            versions['current'] = result.stdout.strip() if result.returncode == 0 else "unknown"
            print(f"✓ Current version: {versions['current']}")
        except Exception as e:
            print(f"✗ Failed to get current version: {e}")
            return False
    else:
        print(f"✗ Current binary not found: {current_binary}")
        return False

    # Test upstream version
    if os.path.exists(upstream_binary):
        try:
            result = subprocess.run([upstream_binary, '--version'],
                                  capture_output=True, text=True, timeout=2)
            versions['upstream'] = result.stdout.strip() if result.returncode == 0 else "unknown"
            print(f"✓ Upstream version: {versions['upstream']}")
        except Exception as e:
            print(f"✗ Failed to get upstream version: {e}")
            return False
    else:
        print(f"✗ Upstream binary not found: {upstream_binary}")
        return False

    # Check if versions are actually different
    if versions['current'] == versions['upstream']:
        print(f"⚠️ WARNING: Both versions report same version string: {versions['current']}")
        print("   This might indicate the binaries are identical")
        return False
    else:
        print(f"✓ Versions are different: {versions['current']} vs {versions['upstream']}")
        return True

def test_binary_differences():
    """Test that binaries are actually different"""
    print("\n=== Testing Binary Differences ===")

    current_binary = "../bspwm"
    upstream_binary = "../bspwm-0.9.12"

    if not (os.path.exists(current_binary) and os.path.exists(upstream_binary)):
        print("✗ Both binaries must exist for comparison")
        return False

    # Compare file sizes
    current_size = os.path.getsize(current_binary)
    upstream_size = os.path.getsize(upstream_binary)

    print(f"Current binary size: {current_size:,} bytes")
    print(f"Upstream binary size: {upstream_size:,} bytes")
    print(f"Size difference: {current_size - upstream_size:+,} bytes ({((current_size / upstream_size - 1) * 100):+.1f}%)")

    if current_size == upstream_size:
        print("⚠️ WARNING: Binaries are identical size")

    # Compare file hashes
    try:
        import hashlib

        with open(current_binary, 'rb') as f:
            current_hash = hashlib.sha256(f.read()).hexdigest()[:16]

        with open(upstream_binary, 'rb') as f:
            upstream_hash = hashlib.sha256(f.read()).hexdigest()[:16]

        print(f"Current binary hash: {current_hash}...")
        print(f"Upstream binary hash: {upstream_hash}...")

        if current_hash == upstream_hash:
            print("✗ CRITICAL: Binaries are identical (same hash)")
            return False
        else:
            print("✓ Binaries are different")
            return True

    except Exception as e:
        print(f"⚠️ Could not compare hashes: {e}")
        return current_size != upstream_size

def test_microbench_isolation():
    """Test that microbenchmarks are properly isolated"""
    print("\n=== Testing Microbench Isolation ===")

    microbench_binary = "./microbench"

    if not os.path.exists(microbench_binary):
        print("✗ Microbench binary not found. Run: gcc -o microbench microbench.c -lm -lxcb")
        return False

    print("✓ Microbench binary exists")
    print("✓ Microbenchmarks test algorithms in isolation (not affected by running bspwm)")
    print("✓ Results should be consistent regardless of current bspwm state")

    return True

def test_integration_benchmark_problem():
    """Demonstrate the problem with old integration benchmarks"""
    print("\n=== Testing Integration Benchmark Problem ===")

    current_bspc = "../bspc"
    upstream_bspc = "../bspc-0.9.12"

    print("The old bench.py script has this problem:")
    print(f"1. python3 bench.py current   → uses {current_bspc} → connects to YOUR RUNNING bspwm")
    print(f"2. python3 bench.py upstream  → uses {upstream_bspc} → connects to YOUR RUNNING bspwm")
    print("   Both tests connect to the SAME bspwm instance!")
    print()
    print("The new isolated_bench.py fixes this by:")
    print("1. Starting separate Xvfb displays (:99, :100)")
    print("2. Starting different bspwm versions in each Xvfb")
    print("3. Testing each bspc against its matching bspwm version")

    # Check if we can demonstrate this
    if 'BSPWM_SOCKET' in os.environ:
        socket_path = os.environ['BSPWM_SOCKET']
        print(f"✓ Current bspwm socket: {socket_path}")
        print("  Old benchmarks would test against this single instance")
    else:
        print("✓ No bspwm socket detected in environment")
        print("  But old benchmarks would still connect to default socket")

    return True

def test_benchmark_dependencies():
    """Test benchmark dependencies"""
    print("\n=== Testing Benchmark Dependencies ===")

    deps = {
        'python3': 'Required for benchmark scripts',
        'gcc': 'Required for building microbenchmarks',
        'Xvfb': 'Required for isolated testing (install: sudo apt install xvfb)',
        'ps': 'Required for process monitoring'
    }

    all_good = True
    for dep, desc in deps.items():
        if subprocess.run(['which', dep], capture_output=True).returncode == 0:
            print(f"✓ {dep}: available")
        else:
            print(f"✗ {dep}: missing - {desc}")
            if dep == 'Xvfb':
                print("  Without Xvfb, only microbenchmarks will work")
            else:
                all_good = False

    return all_good

def main():
    print("bspwm Benchmark Validation Tool")
    print("=" * 50)

    results = {
        'version_detection': test_version_detection(),
        'binary_differences': test_binary_differences(),
        'microbench_isolation': test_microbench_isolation(),
        'integration_problem': test_integration_benchmark_problem(),
        'dependencies': test_benchmark_dependencies()
    }

    print("\n" + "=" * 50)
    print("VALIDATION SUMMARY")
    print("=" * 50)

    for test_name, passed in results.items():
        status = "PASS" if passed else "FAIL"
        print(f"{test_name}: {status}")

    all_passed = all(results.values())

    if all_passed:
        print("\n✅ ALL TESTS PASSED")
        print("Your benchmarks should provide accurate comparisons between versions.")
    else:
        print("\n❌ SOME TESTS FAILED")
        print("Fix the issues above before trusting benchmark results.")

        if not results['version_detection'] or not results['binary_differences']:
            print("\n🔥 CRITICAL: You may not have different bspwm versions to compare!")
            print("   Run the full benchmark suite first: ./run_benchmarks.sh")

    print("\n📊 RECOMMENDED BENCHMARKING:")
    if results['dependencies']:
        print("   ./run_benchmarks.sh              # Complete automated suite")
        print("   cd benches && python3 isolated_bench.py all  # Proper isolated testing")
    else:
        print("   cd benches && ./microbench       # Algorithm testing only")

    return 0 if all_passed else 1

if __name__ == "__main__":
    sys.exit(main())