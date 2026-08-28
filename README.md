# frozen-sched-verify

鸿蒙 PC / Android 通用「后台应用冻结态调度」—— 方案、模拟器、Android 实现、CI 验证全套。

## 目录结构
```
frozen-sched-verify/
├── README.md
├── sim_report.json
├── 课题_鸿蒙PC冻结态调度.md
├── 解法_鸿蒙PC冻结态调度_方案设计.md
├── Android验证实施指南.md
├── 方案与Android实现差距分析.md
├── simulator.py
├── frozen_sched_daemon/
│   ├── Android.bp
│   ├── aidl/
│   │   └── IFrozenScheduler.aidl
│   ├── daemon/
│   │   ├── main.cpp
│   │   ├── Scheduler.cpp
│   │   ├── Scheduler.h
│   │   ├── Freezer.cpp
│   │   ├── Freezer.h
│   │   ├── ZramMonitor.cpp
│   │   ├── ZramMonitor.h
│   │   ├── RdPredictor.cpp
│   │   ├── RdPredictor.h
│   │   ├── NotifProxy.cpp
│   │   └── NotifProxy.h
│   ├── sepolicy/
│   │   └── frozen_sched.te
│   └── init/
│       └── frozen_sched.rc
├── tests/
│   ├── cts/
│   │   ├── test_freeze_thaw.py
│   │   ├── test_mem_budget.py
│   │   └── test_notif_delivery.py
│   └── perf/
│       └── analyze_trace.py
└── .github/
    └── workflows/
        └── cuttlefish_verify.yml
```

## 快速开始（仅需 GitHub 账号）
```bash
git clone https://github.com/<your>/frozen-sched-verify.git
cd frozen-sched-verify
git push origin main   # 触发 GitHub Actions 自动跑 Cuttlefish 验证
```

## 本地跑 Python 模拟器（可选）
```bash
python3 simulator.py
# 输出: ALL PASS ✓
```

## 核心指标（Python 模拟器基准）
| 指标 | 阈值 | 实测 |
|---|---|---|
| T1 状态机非法转换 | 0 | 0 |
| T2 后台/前台内存 P99 | <= 0.10 | 0.05 |
| T3 24h 换出 | <= 100 GB | 32.69 GB |
| T4 通知送达率 min | >= 0.90 | 0.9024 |
| T5 TOP-20 全通过 | true | true |

> **总体：ALL PASS ✓**