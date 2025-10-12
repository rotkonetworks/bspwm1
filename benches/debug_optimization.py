#!/usr/bin/env python3

import subprocess
import time
import os
import signal

def test_basic_bspwm_setup():
    """Test basic bspwm setup in Xvfb"""
    display = ":99"
    xvfb_proc = None
    bspwm_proc = None

    try:
        print("Starting Xvfb...")
        xvfb_proc = subprocess.Popen([
            'Xvfb', display,
            '-screen', '0', '1920x1080x24',
            '-ac'
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)

        print("Starting bspwm...")
        env = os.environ.copy()
        env['DISPLAY'] = display
        env['BSPWM_SOCKET'] = f"/tmp/bspwm-debug-{os.getpid()}"

        bspwm_proc = subprocess.Popen(['../bspwm'],
                                    env=env,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)
        time.sleep(2)

        if bspwm_proc.poll() is not None:
            stdout, stderr = bspwm_proc.communicate()
            print(f"❌ bspwm exited: {bspwm_proc.returncode}")
            print(f"stdout: {stdout.decode()}")
            print(f"stderr: {stderr.decode()}")
            return False

        print("Testing bspc commands...")

        # Test 1: Basic query
        print("  Test 1: bspc query -D")
        result = subprocess.run(['../bspc', 'query', '-D'],
                              env=env, capture_output=True, timeout=5)
        print(f"    Return code: {result.returncode}")
        print(f"    stdout: {repr(result.stdout.decode())}")
        print(f"    stderr: {repr(result.stderr.decode())}")

        if result.returncode == 0:
            print("    ✅ Basic query works!")
        else:
            print("    ❌ Basic query failed")
            return False

        # Test 2: Setup desktops
        print("  Test 2: bspc monitor -d I II III")
        result = subprocess.run(['../bspc', 'monitor', '-d', 'I', 'II', 'III'],
                              env=env, capture_output=True, timeout=5)
        print(f"    Return code: {result.returncode}")
        if result.stderr:
            print(f"    stderr: {result.stderr.decode()}")

        # Test 3: Query after setup
        print("  Test 3: bspc query -D (after setup)")
        result = subprocess.run(['../bspc', 'query', '-D'],
                              env=env, capture_output=True, timeout=5)
        print(f"    Return code: {result.returncode}")
        print(f"    stdout: {repr(result.stdout.decode())}")

        # Test 4: Tree query
        print("  Test 4: bspc query -T")
        result = subprocess.run(['../bspc', 'query', '-T'],
                              env=env, capture_output=True, timeout=5)
        print(f"    Return code: {result.returncode}")
        if result.returncode == 0:
            print(f"    Tree query success (length: {len(result.stdout)})")
        else:
            print(f"    Tree query failed: {result.stderr.decode()}")

        print("✅ All basic tests completed")
        return True

    except Exception as e:
        print(f"❌ Exception: {e}")
        return False

    finally:
        print("Cleaning up...")
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

if __name__ == "__main__":
    test_basic_bspwm_setup()