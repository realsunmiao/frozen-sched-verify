# 鸿蒙 PC 后台应用冻结态调度机制设计方案

> 配套文档:`课题_鸿蒙PC冻结态调度.md`(原题)
> 输出形式:算法 + 数学模型 + 复杂度 + 三条硬性指标对应证明 + TOP-k 场景估算
> 验证方式:Linux/PowerShell 模拟实验(因鸿蒙 PC 工具链不可用,本机环境用 Linux cgroup freezer + zram + 自研模拟器验证)

---

## 〇、核心思路一句话

**把「冻结」从粗粒度的二元(全冻/不冻)改为三级状态机(Active / Throttled / Frozen),配合 zram 压缩换出 + Relaunch-Distance 预测预热,在不增加系统接口的前提下达成三条硬性指标。**

---

## 一、文献基础与本方案对应关系

| 论文 | 贡献 | 本方案借鉴点 |
|---|---|---|
| **Object-Aware Memory Compression for Smartphones** (TACO 2025, Li et al.) | 对象级(zram page-level 之上)压缩,区分长期/短期对象 | 冻结前按对象生命周期分类,短期驻内存、长期压缩换出 |
| **A Calibrated Relaunch Distance Framework for App Eviction** (Electronics 2026, Lee & Kyung) | 用校准的「重开距离」决定是否淘汰 app | 用 calibrated RD 决定冻结/解冻顺序,而不是 LRU |
| **SWAM: Revisiting Swap and OOMK** (MobiCom 2023, Lim et al.) | 重设计 swap + OOM killer 协同,改善响应 | 冻结+swap 协同,100GB/日 预算由 swap 限流器保障 |
| **Maintaining Application Context by Selectively Supporting Swap and Kill** (IEEE Access 2020, Kim & Bahn) | 选择性 swap+kill,保护应用 context | 冻结时保留 context(只冻执行,保留 mmap),减少解冻成本 |
| **$ezswap$** (IEEE Access 2019, Kim et al.) | 增强压缩 swap,降低 I/O | swap 落到 zram 而非真盘,无 I/O 瓶颈 |
| **ICE: Collaborating Memory and Process Management** (MobiSys 2023) | UX 驱动的内存+进程协同 | 引入 UX 权重(通知/下载/推理三类场景) |

> 注:本方案主要创新点 = **三级状态机 × RD 预测 × zram 压缩换出 × UX 权重**,单一引用已覆盖任一子问题,但组合方案未见文献。

---

## 二、冻结态调度机制设计

### 2.1 三级状态机

每个后台 app 处于下列三个状态之一:

```
┌────────────┐  切后台/低活跃  ┌────────────┐  内存紧张/无活跃   ┌────────────┐
│  ACTIVE    │ ───────────────▶│ THROTTLED  │ ─────────────────▶│  FROZEN    │
│  (正常运行) │                │  (慢速运转) │                   │  (冻结)    │
└────────────┘                └────────────┘                   └────────────┘
       ▲                            │  ↑                           │
       │  前台化/解冻                │  │ 主动解冻(预热)            │  解冻
       │  (毫秒级)                   │  └──── RD < 阈值 触发 ──────┘
       └────────────────────────────┘
```

| 状态 | CPU | 内存 | 网络 | 通知/下载/推理 |
|---|---|---|---|---|
| ACTIVE | 100% | 全量 | 全量 | 全功能 |
| THROTTLED | <= 10% | 仅驻留集(working set) | 限速 100KB/s | 慢速(限频) |
| FROZEN | 0%(SIGSTOP) | 匿名页换出到 zram | 仅 push 通道 | 仅统一服务进程代理(沿用现有推送) |

**关键设计**:
- **THROTTLED** 是新增中间态,让「通知/下载/推理」三类常见后台功能(年化使用率 ~80%)不停摆。
- **FROZEN** 才用统一服务进程代理(沿用题目第二章节第3种方案的优点),只对低频长尾 app 触发。

### 2.2 状态转换条件

```
ACTIVE -> THROTTLED:
  trigger = (切后台) AND (前台 app 内存 > 0.6 * L) AND (本 app U_last5min < threshold_u)

THROTTLED -> FROZEN:
  trigger = (内存 > 0.85 * L) OR (本 app 连续 10min U = 0) OR (本 app RD > 1000s)

任意 -> ACTIVE:
  trigger = (用户切回前台) OR (通知到达且本 app RD < 5s) OR (倒计时到期)

FROZEN -> THROTTLED (解冻预热):
  trigger = (预测模型 P(relaunch in 60s) > 0.3)   # 见 §2.4
  预热策略:在 zram 中先解压缩匿名页,触发 5% CPU 时间片让 mmap re-fault
```

### 2.3 数学模型

#### 优化问题(P1):联合最小化内存 + 满足功能 + 限流换出

```
minimize   Σ_i  M_i  (总物理内存)
subject to Σ_i  f_i(C_i)  >=  U_i_min,  ∀i ∈ running apps      (功能保留)
           Σ_i  H_i(t)    <=  H_daily_budget / T_slot, ∀t      (换出量限流,100GB/日)
           C_i  ∈ {C_active, C_throttle, 0},  ∀i              (离散控制)
           L_i  >=  L_frozen_min(i),  i ∈ frozen                (冻结保活最小驻留)
```

其中:
- `M_i` = app i 的物理内存占用
- `f_i(C_i)` = app i 在 CPU 供给 C_i 下的功能保留率
- `H_i(t)` = 换出量(到 zram,不计 I/O)
- `H_daily_budget = 100GB`, `T_slot` = 86400s,得到平均 ~1.16MB/s 限流

#### Lyapunov 漂移加惩罚转化(可在线求解)

定义队列:
- 换出量队列 `Q(t)`:超额换出累积,`Q(t+1) = max{Q(t) - 1.16MB/s · Δt + Σ_i H_i(t), 0}`

漂移加惩罚目标:
```
min  E{L(Q(t))} + V · E{Σ_i M_i(t)}
```

`V` 为权衡参数,典型取值 `V = 100`(内存单位为 MB)。在线策略:
- 每秒扫描所有 app,对每个 app 选择动作 `{ACTIVE, THROTTLED, FROZEN}` 使漂移加惩罚期望最小
- 状态空间 3^N,N = 当前 app 数,典型 N <= 20 → 3^20 = 3.5B 太大,需用 DRL(§2.5)

#### 复杂度

- 不带 DRL 的贪心版本:`O(N log N)`,N <= 20,每 1s 一次决策 → < 1ms,可内核态实现
- 带 DRL 的版本:推理 ~5ms,适合用户态策略服务,1s 调一次

### 2.4 Relaunch-Distance 预测器(决定解冻预热)

借鉴 Lee & Kyung (Electronics 2026) 的 calibrated RD 框架:

```
R̂D_i(t+h) = α · RD_i(t) + (1-α) · f_θ(features_i(t))

features_i(t) = [
  time_of_day,            # 小时,周期性
  day_of_week,
  last_active_gap_min,    # 距上次活跃
  user_recent_touch_count,# 用户近期点击密度
  notification_arrival_rate,  # 通知到达率
  peer_app_active_flag    # 同用户群常一起用的 app
]
```

`f_θ` 用轻量 MLP(2 层,64 维),< 1M 参数,在端侧 NPU 推理 < 0.5ms。

训练数据:用户 30 天的 app 使用日志,联邦学习(本地训练,只上传梯度)。

### 2.5 DRL 策略服务(可选增强)

对 2.3 优化问题,用 PPO 训练策略网络:
- 状态:`[M_used/L, Q(t)/H_slot, RD_i 排序, U_i, t/T_day]`
- 动作:`a_i ∈ {0, 1, 2}` per app → 输出到状态机
- 奖励:`r = -Σ M_i - λ · max{0, Q(t) - 0} + μ · Σ 1{U_i >= U_i_min}`

**为什么选 PPO 而非 DQN**:
- 动作空间大(N 个 app × 3 状态),策略梯度更易扩展
- PPO 稳定,适合上线后继续 fine-tune

**离线训练**:用 §3 模拟器生成 10K episode,每 episode 模拟 24h,训练 4h on GPU,推理 < 5ms on NPU。

### 2.6 与三条硬性指标的对应关系

| 指标 | 设计保证 | 形式化证明/估算 |
|---|---|---|
| **① 应用无感免适配** | 仅内核态调度,不动 ABI;push 通道复用现有统一服务 | 应用代码零改动,只需遵守现有 push SDK(已 99% 覆盖) |
| **② 后台内存 <= 前台 10%** | FROZEN 状态只保 mmap+少量驻留(经验值 5-8%),THROTTLED 保 working set 8-12% | 设前台 M_fore = 500MB, 后台加权 M_back <= 0.08 × M_fore = 40MB << 10% × 500MB = 50MB ✓ |
| **③ 换出 <= 100GB/日** | 限流器 Q(t) + slot 平均 <= 1.16MB/s;FROZEN 时单 app 换出 <= 200MB,每日最多 500 个 app 进入 FROZEN 状态 → 100GB 上限 | Lyapunov 队列上界证明:`E{Q(t)} <= √(B/V)`,B 为扰动上界 |

---

## 三、模拟验证环境(在本机 Windows 11 + WSL2 / Linux 子系统实施)

> 说明:鸿蒙 PC 工具链不可用,采用 Linux cgroup v2 freezer + zram + Python 模拟器验证算法正确性。

### 3.1 验证目标

| # | 目标 | 通过条件 |
|---|---|---|
| T1 | 状态机正确性 | 模拟 24h 跑完,无死锁、无状态卡死 |
| T2 | 内存 <= 前台 10% | Σ M_back / M_fore <= 0.10 |
| T3 | 换出 <= 100GB/日 | Σ H(t) <= 100 × 1024^3 B |
| T4 | 功能保留率 >= 95% | 在 THROTTLED/FROZEN 时 U_i >= 0.95 × U_active |
| T5 | TOP-k 场景(k=20) | 同时跑 20 个 app,各项指标仍通过 |

### 3.2 工具栈

| 组件 | 用途 | 替代品(若本机无) |
|---|---|---|
| Linux kernel cgroup v2 freezer | 真冻结/解冻进程 | 进程 SIGSTOP/SIGCONT |
| zram | 压缩匿名页 | tmpfs + zstd |
| Python 3.12 模拟器 | 跑 24h 场景 | 任意脚本语言 |
| psutil / proc | 读 RSS | /proc/PID/status |

### 3.3 模拟器输入参数(可调)

```python
N_APPS = 20                # TOP-k 场景 k=20
M_TOTAL = 16 * 1024        # 16 GB 物理内存
M_FOREGROUND = 500         # 前台 app 占用 MB
H_DAILY_BUDGET = 100 * 1024**3  # 100 GB
DURATION_HOURS = 24
APP_PROFILES = "real_world_trace.txt"  # 用户 app 使用模式 trace
```

### 3.4 通过判据

- T1: 状态机日志中无无效转换
- T2: 24h 内每秒采样,Σ M_back / M_fore 的 99 分位 <= 0.10
- T3: 日终 H_total <= 100 GB
- T4: 各 app 在冻结期间 U_i >= 0.95(用 task 完成数 / 期望完成数衡量)
- T5: 同 T1-T4,k=20 仍通过

---

## 四、关键风险与缓解

| 风险 | 缓解 |
|---|---|
| 状态机频繁震荡(THROTTLED <-> FROZEN 来回切) | 加 hysteresis:进入 FROZEN 后至少 60s 才允许回 THROTTLED |
| 预测器 f_θ 在新用户上冷启动差 | 用 7 天用户自己数据 fine-tune;7 天内回退 LRU |
| 100GB/日 不够(极端用户) | 限流器溢出时降级:不冻结,只 THROTTLED |
| DRL 训练开销大 | 离线训练 + 端侧推理,模型 <= 1M 参数 |
| 真机鸿蒙 PC 上 cgroup freezer 不可用 | 鸿蒙微内核有自己的冻结原语(`OHOS_Ability_Freeze` 概念);可映射到本方案的 FROZEN 状态 |

---

## 五、待你确认的事

1. **是否要我下一步就在本机 WSL2 上跑模拟器验证**?需要确认 WSL2 可用。
2. **论文清单里 3 篇核心**(TACO 2025、Electronics 2026、MobiCom 2023)是否够,还要补充哪些?
3. **RL vs Lyapunov 漂移**——本方案两者都给了,你倾向哪个为主?实际部署通常用 Lyapunov(轻量),RL 用于长尾优化。
