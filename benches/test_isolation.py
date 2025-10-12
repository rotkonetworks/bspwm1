#!/usr/bin/env python3

import subprocess
import os
import time
import shutil

def test_display_isolation():
    """Test if we can run bspwm on different displays simultaneously"""
    print("Testing display isolation...")

    if not shutil.which('Xvfb'):
        print("❌ Xvfb not found. Install with: sudo pacman -S xorg-server-xvfb")
        return False

    # Check current display
    current_display = os.environ.get('DISPLAY', 'none')
    print(f"Current DISPLAY: {current_display}")

    # Test starting Xvfb on :99
    print("Starting Xvfb on :99...")
    try:
        xvfb = subprocess.Popen([
            'Xvfb', ':99',
            '-screen', '0', '800x600x24',
            '-ac'
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        time.sleep(1)

        if xvfb.poll() is None:  # Still running
            print("✅ Xvfb started successfully on :99")

            # Test if we can run a simple X command
            env = os.environ.copy()
            env['DISPLAY'] = ':99'

            result = subprocess.run(['xdpyinfo'], env=env,
                                  capture_output=True, timeout=5)
            if result.returncode == 0:
                print("✅ Can communicate with Xvfb :99")
                success = True
            else:
                print("❌ Cannot communicate with Xvfb :99")
                success = False
        else:
            print("❌ Xvfb failed to start")
            success = False

        # Cleanup
        xvfb.terminate()
        try:
            xvfb.wait(timeout=2)
        except subprocess.TimeoutExpired:
            xvfb.kill()

        return success

    except Exception as e:
        print(f"❌ Error testing Xvfb: {e}")
        return False

def check_current_bspwm():
    """Check if bspwm is currently running"""
    print("\nChecking current bspwm...")

    try:
        # Check if bspc can connect to running bspwm
        result = subprocess.run(['../bspc', 'query', '-D'],
                              capture_output=True, timeout=2)
        if result.returncode == 0:
            desktops = result.stdout.decode().strip().split('\n')
            print(f"✅ bspwm is running with {len(desktops)} desktops")
            return True
        else:
            print("❌ No running bspwm detected")
            return False
    except Exception as e:
        print(f"❌ Error checking bspwm: {e}")
        return False

def main():
    print("bspwm Isolation Test")
    print("=" * 30)

    results = []

    # Test 1: Display isolation
    results.append(("Display isolation", test_display_isolation()))

    # Test 2: Current bspwm detection
    results.append(("Current bspwm", check_current_bspwm()))

    print("\n" + "=" * 30)
    print("RESULTS:")
    for test_name, passed in results:
        status = "✅ PASS" if passed else "❌ FAIL"
        print(f"{test_name}: {status}")

    if all(result[1] for result in results):
        print("\n🎉 Isolation should work!")
        print("You can run isolated benchmarks while bspwm is running.")
        print("The tests will use separate virtual displays (:99, :100)")
        print("while your desktop stays on the current display.")

        if not shutil.which('Xvfb'):
            print("\n📦 Next step: sudo pacman -S xorg-server-xvfb")
        else:
            print("\n🚀 Ready to run: python3 isolated_bench.py all")
    else:
        print("\n⚠️ Some issues detected.")
        print("Check the failures above before running isolated benchmarks.")

if __name__ == "__main__":
    main()