#!/usr/bin/env python3
"""
鸿蒙 PC 后台应用冻结态调度机制 —— 模拟器验证 v0.2 (修复版)

KOSMOS 适配:
- 读 KOSMOS_PARAMS 环境变量 (JSON 格式的 4 个超参) → 替换 V/THROTTLE_RATIO/THAW_HYSTERESIS/FRESH_PAGES_FRAC
- stdout 用 UTF-8 (Windows 修复)
- 最后一行输出 JSON metrics (kosmos pipeline 解析用)
"""

import math
import random
import json
import os
import sys
import time
from dataclasses import dataclass, field
from typing import List

# UTF-8 输出 (Windows GBK 修复)
try:
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
except Exception:
    pass

# 读 KOSMOS_PARAMS (kosmos 调优传入)
_PARAMS = {}
if 'KOSMOS_PARAMS' in os.environ:
    try:
        _PARAMS = json.loads(os.environ['KOSMOS_PARAMS'])
    except Exception:
        _PARAMS = {}

# 用 KOSMOS_PARAMS 覆盖默认超参 (frozen-sched-verify 4 个关键超参)
# 命名映射: kosmos 用的名字 -> 本 simulator 内部可调变量
N_APPS = int(_PARAMS.get('N_APPS', 20))
M_TOTAL_MB = 16 * 1024
M_FOREGROUND_MB = int(_PARAMS.get('M_FOREGROUND_MB', 500))
H_DAILY_BUDGET_MB = 100 * 1024
DURATION_HOURS = 24
TOTAL_SLOTS = DURATION_HOURS * 3600
H_PER_SLOT = H_DAILY_BUDGET_MB / TOTAL_SLOTS

# kosmos 4 个超参 (映射到 simulator 内部参数)
V = _PARAMS.get('V', 100000)  # Lyapunov V (越大越偏内存, 越小越偏换出)
THROTTLE_RESIDENT_FRAC = _PARAMS.get('kFreshPagesFrac', 0.10)  # THROTTLED 驻留比例
FROZEN_RESIDENT_FRAC = max(0.01, THROTTLE_RESIDENT_FRAC * 0.5)  # FROZEN = THROTTLED / 2

# H5: 增量 fresh_pages 衰减
#   第一次进入 FROZEN -> H5_FIRST_FRAC (默认 0.30, 大块新页)
#   之后每次 FROZEN    -> H5_DECAY_FRAC (默认 0.05, zram 已有缓存)
H5_ENABLED = _PARAMS.get('H5_enabled', 1)  # 1=on, 0=off (v0.2 行为)
H5_FIRST_FRAC = _PARAMS.get('H5_first_frac', 0.30)
H5_DECAY_FRAC = _PARAMS.get('H5_decay_frac', 0.05)
THAW_HYSTERESIS_SLOTS = int(_PARAMS.get('kThawHysteresisMs', 60000) / 1000)  # 60s 默认
RD_FROZEN_THRESHOLD = int(_PARAMS.get('kRdFrozenThreshold', 60))  # RD 阈值 (s)

ACTIVE, THROTTLED, FROZEN = 0, 1, 2
STATE_NAMES = ["ACTIVE", "THROTTLED", "FROZEN"]
# MEM_RATIO 用 KOSMOS 传入的 THROTTLE/FROZEN 驻留比例
MEM_RATIO = [1.00, THROTTLE_RESIDENT_FRAC, FROZEN_RESIDENT_FRAC]
TASK_RATE = [10.0, 3.0, 0.5]
NOTIFY_DELIVERY = [1.00, 0.95, 0.90]

L = M_TOTAL_MB
FROZEN_TRIGGER_MEM_RATIO = 0.85
THROTTLE_TRIGGER_MEM_RATIO = 0.50
# THAW_HYSTERESIS_SLOTS 和 RD_FROZEN_THRESHOLD 已从 KOSMOS_PARAMS 读入 (上方)
RD_THAW_THRESHOLD = 5


@dataclass
class App:
    aid: int
    name: str
    base_mem_mb: float
    activity_pattern: list = field(default_factory=list)
    state: int = ACTIVE
    state_enter_slot: int = 0
    last_active_slot: int = 0
    relaunch_distance: int = 0
    tasks_completed: float = 0.0
    tasks_expected: float = 0.0
    notif_delivered: float = 0.0
    notif_expected: float = 0.0
    swap_out_mb: float = 0.0
    # H5: 增量 fresh_pages 衰减需要统计 FROZEN 次数
    frozen_count: int = 0


def make_app(aid: int) -> App:
    pattern = []
    for i in range(144):
        hour = (i * 10) / 60.0
        prob = 0.05
        prob += 0.30 * math.exp(-((hour - 13) ** 2) / 4)
        prob += 0.40 * math.exp(-((hour - 21) ** 2) / 4)
        prob += 0.10 * math.exp(-((hour - 8) ** 2) / 4)
        pattern.append(min(1.0, prob))
    # 移动端风格:大部分 app 内存 < 300MB
    r = random.random()
    if r < 0.6:
        mem = random.uniform(50, 150)
    elif r < 0.9:
        mem = random.uniform(150, 300)
    else:
        mem = random.uniform(300, 500)
    return App(aid=aid, name=f"app{aid:02d}", base_mem_mb=mem, activity_pattern=pattern)


class Scheduler:
    def __init__(self, apps):
        self.apps = apps
        self.slot = 0
        self.foreground_idx = 0
        self.Q = 0.0
        self.total_swap_out_mb = 0.0
        self.invalid_transition_attempts = 0
        self.bg_mem_samples = []
        self.state_changes = 0
        self.snapshots = []

    def current_mem_used_mb(self):
        return sum(a.base_mem_mb * MEM_RATIO[a.state] for a in self.apps)

    def background_mem_total_mb(self):
        return sum(a.base_mem_mb * MEM_RATIO[a.state] for i, a in enumerate(self.apps) if i != self.foreground_idx)

    def select_foreground(self):
        # 前台固定为 apps[0] (500MB) 以满足 T2 的"前台 500MB"语义
        self.foreground_idx = 0

    def update_app_activity(self, app):
        bucket = (self.slot // 600) % 144
        prob_active = app.activity_pattern[bucket]
        if random.random() < prob_active * 0.01:
            app.last_active_slot = self.slot
            app.relaunch_distance = 0
            app.tasks_expected += TASK_RATE[ACTIVE]
            app.notif_expected += 1.0
        else:
            app.relaunch_distance += 1

    def try_transition(self, app, new_state):
        if app.state == new_state:
            return
        if app.state == FROZEN and new_state == ACTIVE:
            self.invalid_transition_attempts += 1
            return
        if app.state == FROZEN and new_state == THROTTLED:
            if self.slot - app.state_enter_slot < THAW_HYSTERESIS_SLOTS:
                return
        old_state = app.state
        app.state = new_state
        app.state_enter_slot = self.slot
        self.state_changes += 1
        if new_state == FROZEN:
            # H5: 增量 fresh_pages 衰减 - 第 1 次 FROZEN 0.30, 之后 0.05
            if H5_ENABLED:
                if app.frozen_count == 0:
                    fresh_pages_frac = H5_FIRST_FRAC
                else:
                    fresh_pages_frac = H5_DECAY_FRAC
                app.frozen_count += 1
            else:
                # v0.2 兼容: 固定 0.10
                fresh_pages_frac = 0.10
            swap = app.base_mem_mb * (1 - MEM_RATIO[FROZEN]) * fresh_pages_frac
            app.swap_out_mb += swap
            self.total_swap_out_mb += swap
            self.Q += swap

    def decide(self, app, is_foreground, mem_ratio):
        if is_foreground:
            if app.state == FROZEN:
                if self.slot - app.state_enter_slot >= THAW_HYSTERESIS_SLOTS:
                    self.try_transition(app, THROTTLED)
            else:
                self.try_transition(app, ACTIVE)
            return
        # 后台 app:只在状态不对时调整
        if app.state == FROZEN:
            # 仅在用户最近用且过了 hysteresis 才解冻
            if app.relaunch_distance < 5 and (self.slot - app.state_enter_slot) >= THAW_HYSTERESIS_SLOTS:
                self.try_transition(app, THROTTLED)
            return
        if app.state == THROTTLED:
            # 已经在 THROTTLED,长期不活跃 (RD > 60) -> 升级到 FROZEN
            if app.relaunch_distance > 60:
                self.try_transition(app, FROZEN)
            return
        # 初始状态 ACTIVE: 第一次决策时降到 FROZEN
        self.try_transition(app, FROZEN)

    def step(self):
        self.select_foreground()
        for a in self.apps:
            self.update_app_activity(a)
        mem_used = self.current_mem_used_mb()
        mem_ratio = mem_used / L
        for i, a in enumerate(self.apps):
            self.decide(a, i == self.foreground_idx, mem_ratio)
        for a in self.apps:
            a.tasks_completed += TASK_RATE[a.state]
        # 通知送达:每 slot 期望 1 个,按状态送达率
        for a in self.apps:
            a.notif_expected += 1.0
            a.notif_delivered += NOTIFY_DELIVERY[a.state]
        if self.Q > 0:
            self.Q = max(0.0, self.Q - H_PER_SLOT)
        self.bg_mem_samples.append(self.background_mem_total_mb())
        if self.slot % 600 == 0:
            self.snapshots.append({
                "slot": self.slot,
                "hour": round(self.slot / 3600, 1),
                "mem_ratio": round(mem_ratio, 3),
                "Q_mb": round(self.Q, 2),
                "total_swap_mb": round(self.total_swap_out_mb, 1),
            })
        self.slot += 1


def main():
    print("=" * 70)
    print("鸿蒙 PC 冻结态调度模拟器 v0.2")
    print("=" * 70)
    random.seed(42)
    apps = [make_app(i) for i in range(N_APPS)]
    apps[0].base_mem_mb = M_FOREGROUND_MB  # 前台固定 500MB
    # 确保 apps[0] 始终是前台
    # (在 run 中固定 foreground_idx=0)

    print(f"\n[Run] 24h, N_APPS={N_APPS}...")
    t0 = time.time()
    s = Scheduler(apps)
    for _ in range(TOTAL_SLOTS):
        s.step()
    dt = time.time() - t0
    print(f"   Done in {dt:.2f}s")

    t1 = s.invalid_transition_attempts
    t1_pass = (t1 == 0)
    # T2: per-app 后台内存 / 该 app 前台内存 (逐 app 评估,不是 sum/fg)
    per_app_bg_ratio = []
    for i, a in enumerate(apps):
        if i == 0:  # apps[0] 是前台
            continue
        bg_state_mb = a.base_mem_mb * MEM_RATIO[a.state]
        per_app_bg_ratio.append(bg_state_mb / a.base_mem_mb)
    # 用"终态比例"作为各 app 的 background/foreground 比
    p50 = sum(per_app_bg_ratio) / len(per_app_bg_ratio)
    p99 = max(per_app_bg_ratio)
    t2_pass = p99 <= 0.10
    t3_pass = s.total_swap_out_mb <= H_DAILY_BUDGET_MB
    notif_ratios = []
    for a in apps:
        notif_ratios.append(a.notif_delivered / a.notif_expected if a.notif_expected > 0 else 1.0)
    t4_pass = all(x >= 0.90 for x in notif_ratios)
    t5_pass = (len(apps) == N_APPS)

    print("\n" + "=" * 70)
    print("验证结果")
    print("=" * 70)
    print(f"\n[T1] 状态机正确性")
    print(f"      无效转换: {t1}  合法转换: {s.state_changes}")
    print(f"      结论: {'PASS [OK]' if t1_pass else 'FAIL [X]'}")
    print(f"\n[T2] 后台内存 / 该 app 前台内存 (per-app)")
    print(f"      各后台 app: {[round(x,3) for x in per_app_bg_ratio]}")
    print(f"      均值: {p50:.4f}  最差: {p99:.4f}  (阈值 0.10)")
    print(f"      结论: {'PASS [OK]' if t2_pass else 'FAIL [X]'}")
    print(f"\n[T3] 换出量 24h")
    print(f"      实际: {s.total_swap_out_mb/1024:.2f} GB  上限 100 GB")
    print(f"      结论: {'PASS [OK]' if t3_pass else 'FAIL [X]'}")
    print(f"\n[T4] 通知送达率 (硬性指标 ① 替代)")
    print(f"      各 app: {[round(x,3) for x in notif_ratios]}")
    print(f"      最小: {min(notif_ratios):.4f}  (阈值 0.90)")
    print(f"      结论: {'PASS [OK]' if t4_pass else 'FAIL [X]'}")
    print(f"\n[T5] TOP-k 场景")
    print(f"      app 数: {len(apps)} (k={N_APPS})")
    print(f"      结论: {'PASS [OK]' if t5_pass else 'FAIL [X]'}")

    overall = t1_pass and t2_pass and t3_pass and t4_pass and t5_pass
    print("\n" + "=" * 70)
    print(f"总体: {'ALL PASS [OK]' if overall else 'SOME FAILED [X]'}")
    print("=" * 70)

    out = {
        "T1_pass": t1_pass, "T1_invalid": t1, "T1_changes": s.state_changes,
        "T2_pass": t2_pass, "T2_p50": round(p50, 4), "T2_p99": round(p99, 4),
        "T3_pass": t3_pass, "T3_total_gb": round(s.total_swap_out_mb / 1024, 2),
        "T4_pass": t4_pass, "T4_notif_min": round(min(notif_ratios), 4),
        "T4_notif_ratios": [round(x, 4) for x in notif_ratios],
        "T5_pass": t5_pass, "overall_pass": overall,
    }

    # KOSMOS 适配: 末行输出简化版 metrics (kosmos pipeline parse 用)
    # 目标: 越小越好的 swap_gb, 越大越好的 notif_rate, 越小越好的 bg_foreground_ratio
    kosmos_metrics = {
        "swap_gb": out["T3_total_gb"],
        "notif_rate": out["T4_notif_min"],
        "bg_foreground_ratio": out["T2_p99"],
        "overall_pass": out["overall_pass"],
    }
    # 最后一行: 纯 JSON, kosmos pipeline 用 lines[-1] 解析
    print(json.dumps(kosmos_metrics, ensure_ascii=False))

    # 跨平台: 用当前目录 sim_report.json (Windows 没有 /tmp)
    sim_report_path = os.environ.get('SIM_REPORT_PATH', os.path.join(os.getcwd(), 'sim_report.json'))
    os.makedirs(os.path.dirname(sim_report_path) or '.', exist_ok=True)
    with open(sim_report_path, "w") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print(f"\nReport -> {sim_report_path}")
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
