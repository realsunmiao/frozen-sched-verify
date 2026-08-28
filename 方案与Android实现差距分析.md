# 方案与 Android 实现差距分析（评审/答辩专用）

| # | 设计点 | 方案文档《解法_鸿蒙PC冻结态调度_方案设计.md》 | Android 实现 `frozen_sched_daemon` | 差距等级 | 补救方案 | 验收标准 |
|---|---|---|---|---|---|---|
| 1 | **三级状态机**<br>Active / Throttled / Frozen | 纯逻辑状态机，Lyapunov 驱动 | `cgroup v2 freezer` 写 `frozen`/`thawed` + AMS `onFreeze/onThaw` 回调 | ⚠️ 中 | AIDL `IFrozenScheduler.freeze(uid, level)` 暴露给 Shell/CTS | `freeze → thaw` 往返 < 50ms，无非法转换 |
| 2 | **Lyapunov 漂移加惩罚**<br>V=100, Q 队列 | Python float，1s 周期 | C++ 定点数 `int64_t` Q，周期 1s（或 `--time_factor` 加速） | ✅ 低 | 定点数溢出保护 `Q = min(Q, 1<<60)` | 同一输入序列，Q 轨迹相对误差 < 1% |
| 3 | **Relaunch Distance (RD) 预测**<br>2 层 MLP(8→64→1) | 离线训练 + 联邦学习 | TFLite Micro 模型 (<200KB)，输入 8 维（hour, dow, last_gap, touch_cnt, notif_rate, peer_flag, bucket_id, mem_pressure） | ⚠️ 中 | 预置通用模型 `rd_predictor.tflite`（AOSP 预训练 1M 用户 7 天），首周仅推理，第 2 周起本地 1-step SGD 微调 | 首周 MAE < 120s，第 4 周 MAE < 60s |
| 4 | **通知兜底**<br>统一服务进程代理 | 概率公式 | `NotificationListenerService` + 厂商 Push SDK (FCM/华为/小米/OPPO/vivo) 兼容层 | ⚠️ 高 | 抽象 `PushChannel` 接口，运行时加载 5 家实现；兜底走 FCM | 5 家推送送达率均 ≥ 90%，离线 30min 后补发成功率 ≥ 95% |
| 5 | **内存回收**<br>zram + 增量换出 | 0.1× 经验系数 | 读 `/sys/block/zram0/orig_data_size` 与 `compr_data_size`，仅在 **进入 Frozen** 时触发 `zramctl --find --size` 扩容 | ✅ 低 | `ZramMonitor` 每 1s 采样，写入 Lyapunov Q | 实际压缩比 ≥ 2.5×，换出量误差 < 10% |
| 6 | **冻结/解冻原语** | `SIGSTOP/SIGCONT` | `cgroup v2 freezer` → `/sys/fs/cgroup/app_<uid>/freezer.state` 写 `frozen`/`thawed` | ✅ 低 | 需 `CAP_SYS_ADMIN`，在 `init.rc` 给 daemon `capabilities sys_admin` + `seclabel u:r:frozen_sched:s0` | `freeze→thaw` 往返 < 50ms，无僵尸进程 |
| 7 | **生命周期回调** | 无（纯逻辑） | AMS `ActivityManagerService.onFreeze(uid)` / `onThaw(uid)` 埋点 Perfetto | ⚠️ 中 | AOSP 14 已有 `onFreeze`，补 `Trace.traceBegin("onFreeze")`/`Trace.traceEnd()` | Perfetto 抓到 `onFreeze`/`onThaw` 事件，时延 < 5ms |
| 8 | **时间加速 (CI 专用)** | 无 | 守护进程参数 `--time_factor=6`，所有 `sleep`/`timer`/`RD++` ×6 | ✅ 低 | 仅 CI 开启，发版构建去掉 | 10 min 实时 ≈ 1h 逻辑，指标与 sim_report.json 差异 < 5% |
| 9 | **SELinux / Capability** | 未涉及 | `frozen_sched.rc` 里 `capabilities sys_admin` + `seclabel u:r:frozen_sched:s0` + `type frozen_sched, domain;` | ⚠️ 高 | 预编译 `sepolicy` 片段放 `device/<vendor>/sepolicy/frozen_sched.te` | `adb shell ps -Z | grep frozen_sched` 显示正确 label |
| 10 | **联邦学习 (RD 模型更新)** | 本地训练 + 梯度上传 | 暂不落地（v1.0 仅推理），v1.1 引入 `FederatedCompute` | 📦 规划中 | 预留 AIDL `updateModel(byte[] grad)` | 梯度上传 < 1KB/周，用户不感知 |

---

### 评审必问三连击（附标准答案）
1. **“为什么不直接改 lmkd / AMS？”**  
   → 我们在 **userspace daemon 旁路**，通过 AIDL 与 AMS/lmkd 交互，**不改 framework 核心路径**，OTA 热更、回滚安全、厂商定制友好。

2. **“Lyapunov 在 C++ 里定点数会不会精度崩？”**  
   → `Q` 用 `int64_t` 存 `MB * 1000`，V=100000（放大 1000×），单轮漂移 `ΔQ = H - H_per_slot` 精度 1KB，24h 累计误差 < 10MB，远小于 100GB 预算。

3. **“通知兜底 5 家推送 SDK 体积多大？”**  
   → 抽象 `PushChannel` 接口，**运行时动态加载**（`dlopen`），仅加载手机上已安装的那家 SDK，**增量 < 200KB**，不预置任何厂商库。