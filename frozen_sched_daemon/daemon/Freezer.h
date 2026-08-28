#pragma once

#include <mutex>
#include <string>

class Freezer {
public:
    static Freezer& getInstance();
    void freeze(int32_t uid);
    void thaw(int32_t uid);
    bool isFrozen(int32_t uid) const;

private:
    Freezer() = default;
    std::mutex mtx_;
    std::string cgroupPath(int32_t uid) const;
};