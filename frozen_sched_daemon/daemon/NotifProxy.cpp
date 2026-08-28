#include "NotifProxy.h"
#include <android-base/logging.h>
#include <mutex>

NotifProxy& NotifProxy::getInstance() {
    static NotifProxy instance;
    return instance;
}

void NotifProxy::onNotificationPosted(int32_t uid, int64_t posted_ms) {
    std::lock_guard<std::mutex> l(mtx_);
    stats_[uid].posted++;
}

void NotifProxy::onNotificationDelivered(int32_t uid, int64_t delivered_ms) {
    std::lock_guard<std::mutex> l(mtx_);
    stats_[uid].delivered++;
}

float NotifProxy::getDeliveryRate(int32_t uid) const {
    std::lock_guard<std::mutex> l(mtx_);
    auto it = stats_.find(uid);
    if (it == stats_.end() || it->second.posted == 0) return 1.0f;
    return static_cast<float>(it->second.delivered) / it->second.posted;
}