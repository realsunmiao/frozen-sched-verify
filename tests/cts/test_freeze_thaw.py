#!/usr/bin/env python3
"""CTS: freeze/thaw 状态机正确性"""
import subprocess, json, sys, time

def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30)

def aidl_call(method, *args):
    arg_str = " ".join(str(a) for a in args)
    return run(f"adb shell cmd frozen_sched {method} {arg_str}")

def test_freeze_thaw():
    print("[TEST] freeze/thaw roundtrip")
    uid = 10100
    r = aidl_call("getState", uid)
    state = int(r.stdout.strip())
    print(f"  Initial state: {state}")
    assert state in (0, -1), f"unexpected initial state {state}"

    r = aidl_call("freeze", uid, 2)
    assert r.returncode == 0, f"freeze failed: {r.stderr}"
    time.sleep(0.1)
    r = aidl_call("getState", uid)
    assert int(r.stdout.strip()) == 2, "should be FROZEN"
    print("  -> FROZEN OK")

    r = aidl_call("thaw", uid)
    assert r.returncode == 0, f"thaw failed: {r.stderr}"
    time.sleep(0.1)
    r = aidl_call("getState", uid)
    assert int(r.stdout.strip()) == 0, "should be ACTIVE after thaw"
    print("  -> ACTIVE OK")

    r = aidl_call("freeze", uid, 2)
    time.sleep(0.1)
    r = aidl_call("freeze", uid, 0)
    time.sleep(0.1)
    r = aidl_call("getState", uid)
    state = int(r.stdout.strip())
    assert state == 2, f"illegal FROZEN->ACTIVE should be rejected, got {state}"
    print("  -> Illegal transition rejected OK")

    print("[PASS] freeze/thaw")
    return True

if __name__ == "__main__":
    try:
        test_freeze_thaw()
    except Exception as e:
        print(f"[FAIL] {e}")
        sys.exit(1)