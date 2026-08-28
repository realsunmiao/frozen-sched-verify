#include "Freezer.h"
#include <android-base/logging.h>
#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <mutex>

using namespace android::base;

Freezer& Freezer::getInstance() {
    static Freezer instance;
    return instance;
}

std::string Freezer::cgroupPath(int32_t uid) const {
    return StringPrintf("/sys/fs/cgroup/app_%d/freezer.state", uid);
}

void Freezer::freeze(int32_t uid) {
    std::lock_guard<std::mutex> l(mtx_);
    std::string path = cgroupPath(uid);
    if (!WriteStringToFile("frozen\n", path)) {
        PLOG(ERROR) << "Failed to freeze UID=" << uid << " path=" << path;
    } else {
        LOG(DEBUG) << "Frozen UID=" << uid;
    }
}

void Freezer::thaw(int32_t uid) {
    std::lock_guard<std::mutex> l(mtx_);
    std::string path = cgroupPath(uid);
    if (!WriteStringToFile("thawed\n", path)) {
        PLOG(ERROR) << "Failed to thaw UID=" << uid << " path=" << path;
    } else {
        LOG(DEBUG) << "Thawed UID=" << uid;
    }
}

bool Freezer::isFrozen(int32_t uid) const {
    std::lock_guard<std::mutex> l(mtx_);
    std::string content;
    if (ReadFileToString(cgroupPath(uid), &content)) {
        return content.find("frozen") != std::string::npos;
    }
    return false;
}