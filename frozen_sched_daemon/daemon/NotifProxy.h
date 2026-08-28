#pragma once

#include <mutex>
#include <unordered_map>

class NotifProxy {
public:
    static NotifProxy& getInstance();
    void onNotificationPosted(int32_t uid, int64_t posted_ms);
    void onNotificationDelivered(int32_t uid, int64_t delivered_ms);
    float getDeliveryRate(int32_t uid) const;

private:
    NotifProxy() = default;
    struct Stats {
        int64_t posted = 0;
        int64_t delivered = 0;
    };
    mutable std::mutex mtx_;
    std::unordered_map<int32_t, Stats> stats_;
};