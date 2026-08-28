#!/usr/bin/env python3
"""CTS: 内存预算 & 换出量 <= 100GB/日"""
import subprocess, json, sys, time, re

def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30)

def aidl_call(method, *args):
    arg_str = " ".join(str(a) for a in args)
    return run(f"adb shell cmd frozen_sched {method} {arg_str}")

def test_mem_budget():
    print("[TEST] memory budget & swap budget")
    r = aidl_call("dump")
    assert r.returncode == 0, r.stderr
    print(r.stdout)
    m = re.search(r"TotalSwapOut=(\d+)\s*MB", r.stdout)
    assert m, "dump missing TotalSwapOut"
    swap_mb = int(m.group(1))
    print(f"  Current swap out: {swap_mb} MB")
    assert swap_mb <= 102400, f"swap {swap_mb} MB exceeds 100 GB budget"
    print(f"  Swap budget OK (<= 102400 MB)")
    print("[PASS] memory budget smoke test")
    return True

if __name__ == "__main__":
    try:
        test_mem_budget()
    except Exception as e:
        print(f"[FAIL] {e}")
        sys.exit(1)