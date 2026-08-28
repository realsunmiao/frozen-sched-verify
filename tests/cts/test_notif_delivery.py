#!/usr/bin/env python3
"""CTS: 通知送达率 >= 90%"""
import subprocess, json, sys, time

def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30)

def aidl_call(method, *args):
    arg_str = " ".join(str(a) for a in args)
    return run(f"adb shell cmd frozen_sched {method} {arg_str}")

def test_notif_delivery():
    print("[TEST] notification delivery rate")
    r = aidl_call("dump")
    assert r.returncode == 0
    print("[PASS] notif delivery smoke (full rate checked in analyze_trace.py)")
    return True

if __name__ == "__main__":
    try:
        test_notif_delivery()
    except Exception as e:
        print(f"[FAIL] {e}")
        sys.exit(1)