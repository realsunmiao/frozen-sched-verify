# Android 冻结态调度 — 宿主机免配置验证指南 (GitHub Actions + Cuttlefish)

> 目标：在 **纯 Windows/Linux/macOS 宿主机** 上，零本地环境依赖，通过 GitHub Actions 自动完成：
> 1. 启动 Cuttlefish (x86_64, Android 14, API 34)
> 2. 编译 & 推送 `frozen_sched_daemon`
> 3. 运行 CTS 子集（冻结/解冻、内存预算、通知送达）
> 4. 抓 1h Perfetto trace
> 5. 生成 `report_android.json` 并与本地 `sim_report.json` 对标（核心指标差异 < 5% 即过）

---

## 前置条件（只需一次）
- 一个 GitHub 仓库（Public 或 Private 均可，Actions 分钟数足够）
- 仓库里已有以下文件：
  ```
  frozen_sched_daemon/        # C++ 源码 + Android.bp
  .github/workflows/cuttlefish_verify.yml
  sim_report.json             # 本地 Python 模拟器的基准报告
  ```

---

## 一键触发流程
1. `git push origin main`  → Actions 自动跑
2. 等待 ~12 min（Cuttlefish 启动 ~4 min + 编译推送 ~3 min + 1h trace 采集被压缩为 **10 min 加速模拟**）
3. Artifacts 下载：
   - `report_android.json`（关键指标 JSON）
   - `frozen_1h.pftrace`（Perfetto 可视化）
   - `diff_report.txt`（与 `sim_report.json` 差异表）

---

## 为什么 1h trace 只跑 10 min？
- CI 分钟数宝贵，利用 **时间加速因子 `time_factor=6`**：
  - 在 `frozen_sched_daemon` 启动参数里加 `--time_factor=6`
  - 内部所有 `sleep(1s)`、`Lyapunov 周期 1s`、RD 计数器全部 ×6
  - 10 min 实时 ≈ 1h 逻辑时间，指标具备可比性
- 正式发版前可再跑一次全真 24h（需自备机器），但 CI 门槛用加速版即可。

---

## 关键指标对标表（自动生成 `diff_report.txt`）

| 指标 | sim_report.json | report_android.json | 允许偏差 | 判定 |
|---|---|---|---|---|
| T1 状态机非法转换 | 0 | 0 | 0 | ✅ |
| T2 后台/前台内存 P99 | 0.05 | ≤ 0.06 | +0.01 | ✅ |
| T3 24h 换出 (GB) | 32.69 | ≤ 35 | +10% | ✅ |
| T4 通知送达率 min | 0.9024 | ≥ 0.88 | -0.02 | ✅ |
| T5 TOP-20 全通过 | true | true | - | ✅ |

> 任一 ❌ → Actions 标红，PR 不可合并。

---

## 常见失败 & 1 分钟自查
| 现象 | 原因 | 1 分钟修复 |
|---|---|---|
| `cvd start` 超时 | GH runner 内存 < 16 GB | workflow 里改 `machine: ubuntu-latest-16gb` 或 `cvd start -gpu_mode=guest_swiftshader -memory_mb=12288` |
| `adb devices` 显示 `unauthorized` | `adb root` 太早 | 在 `cvd start` 后加 `sleep 30` 再 `adb root` |
| `frozen_sched_daemon` 启动报 `permission denied` | SELinux 阻拦 | `.rc` 里加 `seclabel u:r:frozen_sched:s0` + `capabilities sys_admin` |
| Perfetto 抓不到 `onFreeze` 事件 | AMS 没埋点 | 确认 `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java` 已打 `Trace.traceBegin("onFreeze")`（AOSP 14 自带） |

---

## 产物归档位置
- GitHub Actions **Artifacts** 保留 30 天
- Release 页面自动打包 `report_android.json` + `frozen_1h.pftrace` + `diff_report.txt`，版本号 = `vYYYYMMDD-HHMM`