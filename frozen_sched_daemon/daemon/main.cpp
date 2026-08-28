#include <aidl/android/hardware/frozensched/IFrozenScheduler.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android-base/logging.h>
#include <Scheduler.h>
#include <getopt.h>
#include <unistd.h>

using namespace android::hardware::frozensched;

int main(int argc, char** argv) {
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));

    int time_factor = 1;
    int opt;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == 't') time_factor = atoi(optarg);
    }

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    // Total memory from /proc/meminfo
    int64_t total_mem_kb = 16 * 1024 * 1024; // default 16GB
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                total_mem_kb = atoll(line + 9);
                break;
            }
        }
        fclose(f);
    }

    int64_t daily_budget_kb = 100 * 1024 * 1024; // 100 GB

    android::sp<Scheduler> scheduler = new Scheduler(total_mem_kb, daily_budget_kb, time_factor);

    // Register AIDL service
    const char* instance = "frozen_sched/default";
    binder_status_t status = AServiceManager_addService(scheduler->asBinder().get(), instance);
    if (status != STATUS_OK) {
        LOG(FATAL) << "Failed to register AIDL service: " << status;
    }
    LOG(INFO) << "frozen_sched_daemon registered as " << instance << ", time_factor=" << time_factor;

    // Run scheduler loop (blocks forever)
    scheduler->run();
    return 0;
}