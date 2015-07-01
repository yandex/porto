#pragma once

#include "util/log.hpp"

constexpr bool AllowHolderUnlock = false;

typedef std::unique_lock<std::mutex> TScopedLock;

class TScopedUnlock : public TNonCopyable {
public:
    TScopedUnlock(TScopedLock &lock) {
        PORTO_ASSERT(lock);
        Lock = &lock;
        Lock->unlock();
    }
    ~TScopedUnlock() {
        PORTO_ASSERT(!*Lock);
        Lock->lock();
    }
private:
    TScopedLock *Lock;
};

class TLockable {
public:
    TScopedLock ScopedLock() {
        return TScopedLock(Mutex);
    }
    TScopedLock NestScopedLock(TScopedLock &lock) {
        PORTO_ASSERT(lock);
        TScopedUnlock unlock(lock);
        return TScopedLock(Mutex);
    }
private:
    std::mutex Mutex;
};
