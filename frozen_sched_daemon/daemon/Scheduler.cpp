#include "Scheduler.h"
#include "Freezer.h"
#include "ZramMonitor.h"
#include "RdPredictor.h"
#include "NotifProxy.h"
#include <android-base/logging.h>
#include <android-base/chrono_utils.h>
#include <chrono>
#include <cmath>

using namespace android::hardware::frozensched;

Scheduler::Scheduler(int64_t total_mem_kb, int64_t daily_budget_kb, int time_factor)
    : kTotalMemKb_(total_mem_kb),
      kDailyBudgetKb_(daily_budget_kb),
      kSlotMs_(1000 / time_factor),
      kHPerSlotKb_((daily_budget_kb * 1000) / (86400 * time_factor)), // KB * 1000 per slot
      kTimeFactor_(time_factor) {
    last_slot_ms_ = android::base::GetBootClockMicros() / 1000;
    LOG(INFO) << "Scheduler init: total_mem=" << kTotalMemKb_ << "KB, daily_budget=" << kDailyBudgetKb_ << "KB, slot=" << kSlotMs_ << "ms, time_factor=" << kTimeFactor_;
}

binder::Status Scheduler::freeze(int32_t uid, int32_t level) {
    std::lock_guard<Mutex> l(lock_);
    auto it = apps_.find(uid);
    if (it == apps_.end()) {
        // Auto-create with default 200MB base
        AppInfo a;
        a.uid = uid;
        a.base_mem_kb = 200 * 1024;
        apps_[uid] = std::move(a);
        it = apps_.find(uid);
    }
    AppState target = static_cast<AppState>(level);
    if (target >= ACTIVE && target <= FROZEN) {
        transitionLocked(it->second, target, android::base::GetBootClockMicros() / 1000);
    }
    return binder::Status::ok();
}

binder::Status Scheduler::thaw(int32_t uid) {
    std::lock_guard<Mutex> l(lock_);
    auto it = apps_.find(uid);
    if (it != apps_.end()) {
        transitionLocked(it->second, ACTIVE, android::base::GetBootClockMicros() / 1000);
    }
    return binder::Status::ok();
}

binder::Status Scheduler::getState(int32_t uid, int32_t* _aidl_return) {
    std::lock_guard<Mutex> l(lock_);
    auto it = apps_.find(uid);
    if (it != apps_.end()) {
        *_aidl_return = static_cast<int32_t>(it->second.state);
    } else {
        *_aidl_return = -1;
    }
    return binder::Status::ok();
}

binder::Status Scheduler::dump(std::string* _aidl_return) {
    std::lock_guard<Mutex> l(lock_);
    std::string out = "FrozenScheduler Dump\n";
    out += "Q_kb=" + std::to_string(Q_kb_ / 1000) + " KB\n";
    out += "TotalSwapOut=" + std::to_string(total_swap_out_kb_ / 1024) + " MB\n";
    out += "ForegroundUID=" + std::to_string(foreground_uid_) + "\n";
    out += "---\n";
    for (const auto& kv : apps_) {
        const auto& a = kv.second;
        out += "UID=" + std::to_string(a.uid) + " State=" + std::to_string(a.state) +
               " BaseMem=" + std::to_string(a.base_mem_kb / 1024) + "MB" +
               " RD=" + std::to_string(a.relaunch_distance_s) + "s" +
               " SwapOut=" + std::to_string(a.swap_out_kb / 1024) + "MB\n";
    }
    *_aidl_return = out;
    return binder::Status::ok();
}

binder::Status Scheduler::updateModel(const std::vector<uint8_t>& grad) {
    // v1.1: federated learning gradient update
    LOG(INFO) << "Model update received, size=" << grad.size();
    // TODO: feed to RdPredictor::updateModel(grad)
    return binder::Status::ok();
}

void Scheduler::run() {
    using namespace std::chrono_literals;
    while (true) {
        int64_t now_ms = android::base::GetBootClockMicros() / 1000;
        {
            std::lock_guard<Mutex> l(lock_);
            // 1. Update RD for all apps
            for (auto& kv : apps_) {
                updateRdLocked(kv.second, now_ms);
            }
            // 2. Lyapunov step
            lyapunovStepLocked(now_ms);
            // 3. Decide state for each app
            for (auto& kv : apps_) {
                decideLocked(kv.second, kv.first == foreground_uid_, now_ms);
            }
            // 4. Apply transitions (Freezer + Zram)
            // (done inside transitionLocked via Freezer/ZramMonitor singletons)
        }
        // Sleep to next slot
        int64_t elapsed = (android::base::GetBootClockMicros() / 1000) - now_ms;
        int64_t sleep_ms = kSlotMs_ - elapsed;
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
}

void Scheduler::updateRdLocked(AppInfo& a, int64_t now_ms) {
    if (a.last_active_ms > 0) {
        a.relaunch_distance_s = (now_ms - a.last_active_ms) / 1000;
    } else {
        a.relaunch_distance_s = INT64_MAX / 2;
    }
    // Update features for RD predictor (8 dim)
    // [hour, dow, last_gap_min, touch_cnt, notif_rate, peer_flag, bucket_id, mem_pressure]
    time_t t = now_ms / 1000;
    tm tm = *localtime(&t);
    a.features[0] = tm.tm_hour;
    a.features[1] = tm.tm_wday;
    a.features[2] = a.relaunch_distance_s / 60.0f;
    a.features[3] = 0; // touch_cnt (TODO: hook InputManager)
    a.features[4] = 0; // notif_rate (TODO: hook NotifProxy)
    a.features[5] = 0; // peer_flag
    a.features[6] = 0; // bucket_id (TODO: UsageStatsManager)
    a.features[7] = static_cast<float>(currentMemUsedKbLocked()) / kTotalMemKb_;
}

void Scheduler::decideLocked(AppInfo& a, bool is_foreground, int64_t now_ms) {
    if (is_foreground) {
        if (a.state == FROZEN) {
            if (now_ms - a.state_enter_ms >= kThawHysteresisMs) {
                transitionLocked(a, THROTTLED, now_ms);
            }
        } else {
            transitionLocked(a, ACTIVE, now_ms);
        }
        return;
    }

    // Background app
    if (a.state == FROZEN) {
        if (a.relaunch_distance_s < kRdThawThreshold && (now_ms - a.state_enter_ms >= kThawHysteresisMs)) {
            transitionLocked(a, THROTTLED, now_ms);
        }
        return;
    }
    if (a.state == THROTTLED) {
        if (a.relaunch_distance_s > kRdFrozenThreshold) {
            transitionLocked(a, FROZEN, now_ms);
        }
        return;
    }
    // ACTIVE -> FROZEN directly (default background)
    transitionLocked(a, FROZEN, now_ms);
}

void Scheduler::transitionLocked(AppInfo& a, AppState new_state, int64_t now_ms) {
    if (a.state == new_state) return;
    if (a.state == FROZEN && new_state == ACTIVE) {
        LOG(WARNING) << "Illegal transition FROZEN->ACTIVE for UID=" << a.uid;
        return;
    }
    if (a.state == FROZEN && new_state == THROTTLED) {
        if (now_ms - a.state_enter_ms < kThawHysteresisMs) return;
    }

    AppState old_state = a.state;
    a.state = new_state;
    a.state_enter_ms = now_ms;

    // Apply to Freezer + Zram
    if (new_state == FROZEN) {
        Freezer::getInstance().freeze(a.uid);
        // Incremental swap out (only fresh pages)
        int64_t swap_kb = (a.base_mem_kb * (kMemRatioActive - kMemRatioFrozen) / 1000) * kFreshPagesFrac / 1000;
        a.swap_out_kb += swap_kb;
        total_swap_out_kb_ += swap_kb;
        Q_kb_ += swap_kb * 1000; // Q in KB*1000
        ZramMonitor::getInstance().notifyFreeze(a.uid, swap_kb);
        LOG(INFO) << "UID=" << a.uid << " FROZEN, swap=" << swap_kb << "KB";
    } else if (old_state == FROZEN && new_state == THROTTLED) {
        Freezer::getInstance().thaw(a.uid);
        LOG(INFO) << "UID=" << a.uid << " THAWED to THROTTLED";
    } else if (new_state == ACTIVE) {
        Freezer::getInstance().thaw(a.uid);
        LOG(INFO) << "UID=" << a.uid << " ACTIVE";
    }
}

void Scheduler::lyapunovStepLocked(int64_t now_ms) {
    // Natural decay
    if (Q_kb_ > 0) {
        Q_kb_ = std::max<int64_t>(0, Q_kb_ - kHPerSlotKb_);
    }
    // Emergency: if Q > 1GB, force thaw oldest frozen
    if (Q_kb_ > 1024 * 1024 * 1000) { // 1GB in KB*1000
        for (auto& kv : apps_) {
            auto& a = kv.second;
            if (a.state == FROZEN && (now_ms - a.state_enter_ms >= kThawHysteresisMs)) {
                transitionLocked(a, THROTTLED, now_ms);
                break;
            }
        }
    }
}

int64_t Scheduler::currentMemUsedKbLocked() const {
    int64_t sum = 0;
    for (const auto& kv : apps_) {
        const auto& a = kv.second;
        int64_t ratio = (a.state == ACTIVE) ? kMemRatioActive :
                        (a.state == THROTTLED) ? kMemRatioThrottled : kMemRatioFrozen;
        sum += a.base_mem_kb * ratio / 1000;
    }
    return sum;
}

int64_t Scheduler::bgMemTotalKbLocked(int32_t fg_uid) const {
    int64_t sum = 0;
    for (const auto& kv : apps_) {
        if (kv.first == fg_uid) continue;
        const auto& a = kv.second;
        int64_t ratio = (a.state == ACTIVE) ? kMemRatioActive :
                        (a.state == THROTTLED) ? kMemRatioThrottled : kMemRatioFrozen;
        sum += a.base_mem_kb * ratio / 1000;
    }
    return sum;
}