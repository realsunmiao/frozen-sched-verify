#pragma once

#include <mutex>
#include <unordered_map>

class ZramMonitor {
public:
    static ZramMonitor& getInstance();
    void notifyFreeze(int32_t uid, int64_t swap_kb);
    int64_t getCompressedKb() const;
    int64_t getOriginalKb() const;
    float getCompressionRatio() const;

private:
    ZramMonitor() = default;
    void readZramStats();
    mutable std::mutex mtx_;
    int64_t orig_kb_ = 0;
    int64_t comp_kb_ = 0;
    std::unordered_map<int32_t, int64_t> per_uid_swap_kb_;
};