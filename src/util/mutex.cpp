#include "mutex.hpp"
#include "unix.hpp"
#include "log.hpp"

#include <sys/file.h>

class MutexTimer {
    const std::string &Name;
    uint64_t StartTime;

public:
    MutexTimer(const std::string &name) : Name(name) {
        Statistics->LockOperationsCount++;
        StartTime = GetCurrentTimeMs();
    }

    ~MutexTimer() {
        uint64_t requestTime = GetCurrentTimeMs() - StartTime;
        if (requestTime > 1000) {
            L("Long lock {} operation time={} ms", Name, requestTime);
            Statistics->LockOperationsLonger1s++;
        }
        if (requestTime > 3000)
            Statistics->LockOperationsLonger3s++;
        if (requestTime > 30000)
            Statistics->LockOperationsLonger30s++;
        if (requestTime > 300000)
            Statistics->LockOperationsLonger5m++;
    }
};


MeasuredMutex::MeasuredMutex(const std::string &name) : Name(name) {}

void MeasuredMutex::lock() {
    MutexTimer timer(Name);
    std::mutex::lock();
}

std::unique_lock<std::mutex> MeasuredMutex::UniqueLock() {
    MutexTimer timer(Name);
    return std::unique_lock<std::mutex>(*this);
}


TFileMutex::TFileMutex(const TPath &path, int flags) {
    TError error = File.Open(path, flags);
    if (!error) {
        int ret = flock(File.Fd, LOCK_EX);
        if (ret)
            L_WRN("cannot flock lock {} {}", File.RealPath(), ret);
    } else {
        L_WRN("cannot open {} {}", path, error);
    }
}

TFileMutex::~TFileMutex() {
    if (!File)
        return;

    int ret = flock(File.Fd, LOCK_UN);
    if (ret)
        L_WRN("cannot flock unlock {} {}", File.RealPath(), ret);
}
