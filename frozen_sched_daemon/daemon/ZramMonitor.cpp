#include "ZramMonitor.h"
#include <android-base/logging.h>
#include <android-base/file.h>
#include <android-base/strings.h>
#include <fstream>
#include <mutex>

using namespace android::base;

ZramMonitor& ZramMonitor::getInstance() {
    static ZramMonitor instance;
    return instance;
}

void ZramMonitor::readZramStats() {
    std::lock_guard<std::mutex> l(mtx_);
    std::string content;
    if (ReadFileToString("/sys/block/zram0/stat", &content)) {
        // stat format: orig_data_size compr_data_size ...
        std::vector<std::string> parts = Split(content, " \t\n\r");
        if (parts.size() >= 2) {
            orig_kb_ = std::stoll(parts[0]) / 1024;
            comp_kb_ = std::stoll(parts[1]) / 1024;
        }
    }
}

void ZramMonitor::notifyFreeze(int32_t uid, int64_t swap_kb) {
    std::lock_guard<std::mutex> l(mtx_);
    per_uid_swap_kb_[uid] += swap_kb;
    readZramStats(); // refresh
}

int64_t ZramMonitor::getCompressedKb() const {
    const_cast<ZramMonitor*>(this)->readZramStats();
    return comp_kb_;
}

int64_t ZramMonitor::getOriginalKb() const {
    const_cast<ZramMonitor*>(this)->readZramStats();
    return orig_kb_;
}

float ZramMonitor::getCompressionRatio() const {
    int64_t o = getOriginalKb();
    int64_t c = getCompressedKb();
    return (o > 0) ? static_cast<float>(o) / c : 1.0f;
}