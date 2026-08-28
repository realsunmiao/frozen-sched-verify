#pragma once

#include <aidl/android/hardware/frozensched/IFrozenScheduler.h>
#include <binder/IBinder.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>
#include <utils/Mutex.h>
#include <utils/Vector.h>
#include <cstdint>
#include <unordered_map>
#include <string>

namespace android {
namespace hardware {
namespace frozensched {

enum AppState : int32_t {
    ACTIVE = 0,
    THROTTLED = 1,
    FROZEN = 2,
};

struct AppInfo {
    int32_t uid;
    int64_t base_mem_kb;        // 前台时内存 (KB)
    AppState state = ACTIVE;
    int64_t state_enter_ms = 0; // 进入当前状态的时间戳
    int64_t relaunch_distance_s = 0; // 秒
    int64_t last_active_ms = 0;
    int64_t swap_out_kb = 0;
    // RD predictor features (8 dim)
    float features[8] = {0};
};

class Scheduler : public ::android::RefBase {
public:
    Scheduler(int64_t total_mem_kb, int64_t daily_budget_kb, int time_factor = 1);
    ~Scheduler() = default;

    // AIDL callbacks
    binder::Status freeze(int32_t uid, int32_t level);
    binder::Status thaw(int32_t uid);
    binder::Status getState(int32_t uid, int32_t* _aidl_return);
    binder::Status dump(std::string* _aidl_return);
    binder::Status updateModel(const std::vector<uint8_t>& grad);

    // Main loop (called from main.cpp)
    void run();

private:
    // State machine
    void transitionLocked(AppInfo& a, AppState new_state, int64_t now_ms);
    void decideLocked(AppInfo& a, bool is_foreground, int64_t now_ms);
    void updateRdLocked(AppInfo& a, int64_t now_ms);

    // Lyapunov
    void lyapunovStepLocked(int64_t now_ms);

    // Helpers
    int64_t currentMemUsedKbLocked() const;
    int64_t bgMemTotalKbLocked(int32_t fg_uid) const;

    // Members
    const int64_t kTotalMemKb_;
    const int64_t kDailyBudgetKb_;
    const int64_t kSlotMs_;
    const int64_t kHPerSlotKb_;
    const int32_t kTimeFactor_;

    mutable Mutex lock_;
    std::unordered_map<int32_t, AppInfo> apps_;
    int32_t foreground_uid_ = -1;
    int64_t Q_kb_ = 0;              // Lyapunov queue (KB * 1000 for fixed-point)
    int64_t last_slot_ms_ = 0;
    int64_t total_swap_out_kb_ = 0;

    // Constants (scaled by 1000 for fixed-point)
    static constexpr int64_t kV = 100000;       // V * 1000
    static constexpr int64_t kMemRatioActive = 1000;
    static constexpr int64_t kMemRatioThrottled = 100;   // 10%
    static constexpr int64_t kMemRatioFrozen = 50;       // 5%
    static constexpr int64_t kFreezeTriggerMemRatio = 850; // 85%
    static constexpr int64_t kThrottleTriggerMemRatio = 500; // 50%
    static constexpr int64_t kRdFrozenThreshold = 60;    // seconds
    static constexpr int64_t kRdThawThreshold = 5;       // seconds
    static constexpr int64_t kThawHysteresisMs = 60000;  // 60s
    static constexpr int64_t kFreshPagesFrac = 100;      // 10% * 1000
};
}  // namespace frozensched
}  // namespace hardware
}  // namespace android