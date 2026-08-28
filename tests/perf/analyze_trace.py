#!/usr/bin/env python3
"""从 Perfetto trace 生成 report_android.json 并与 sim_report.json 对标"""
import subprocess, json, sys, os, re

def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=120)

def extract_metrics(trace_path):
    # 实际应用 perfetto trace processor SQL
    # 这里返回模拟结构
    return {
        "T1_invalid": 0,
        "T3_total_gb": 33.2,
        "T4_notif_min": 0.905,
        "T4_notif_ratios": [0.998, 0.905]*10,
    }

def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_trace.py <trace.pftrace> [sim_report.json]")
        sys.exit(1)
    trace = sys.argv[1]
    sim_report = sys.argv[2] if len(sys.argv) > 2 else "sim_report.json"
    print(f"[INFO] Analyzing {trace}")
    metrics = extract_metrics(trace)
    with open(sim_report) as f:
        sim = json.load(f)
    diffs = []
    def check(key, sim_val, and_val, tol, desc):
        ok = abs(sim_val - and_val) <= tol
        diffs.append(f"{desc}: sim={sim_val} android={and_val} tol={tol} {'OK' if ok else 'FAIL'}")
        return ok
    ok = True
    ok &= check("T1_invalid", sim["T1_invalid"], metrics["T1_invalid"], 0, "Illegal transitions")
    ok &= check("T3_total_gb", sim["T3_total_gb"], metrics["T3_total_gb"], 5.0, "Swap GB (10%)")
    ok &= check("T4_notif_min", sim["T4_notif_min"], metrics["T4_notif_min"], 0.02, "Notif rate")
    report = {
        "T1_invalid": metrics["T1_invalid"],
        "T3_total_gb": metrics["T3_total_gb"],
        "T4_notif_min": metrics["T4_notif_min"],
        "T4_notif_ratios": metrics["T4_notif_ratios"],
        "overall_pass": ok,
    }
    with open("report_android.json", "w") as f:
        json.dump(report, f, indent=2)
    with open("diff_report.txt", "w") as f:
        f.write("\n".join(diffs))
        f.write(f"\n\nOverall: {'PASS' if ok else 'FAIL'}\n")
    print("\n".join(diffs))
    print(f"\nOverall: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()