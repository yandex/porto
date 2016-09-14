#pragma once

#include <mutex>

typedef std::unique_lock<std::mutex> TScopedLock;

class TLockable {
public:
    TScopedLock ScopedLock() {
        return TScopedLock(Mutex);
    }
private:
    std::mutex Mutex;
};
