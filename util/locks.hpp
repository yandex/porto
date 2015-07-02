#pragma once

#include "util/log.hpp"

constexpr bool AllowHolderUnlock = false;

typedef std::unique_lock<std::mutex> TScopedLock;

class TScopedUnlock : public TNonCopyable {
public:
    TScopedUnlock(TScopedLock &lock) {
        if (AllowHolderUnlock) {
            PORTO_ASSERT(lock);
            Lock = &lock;
            Lock->unlock();
        }
    }
    ~TScopedUnlock() {
        if (AllowHolderUnlock) {
            PORTO_ASSERT(!*Lock);
            Lock->lock();
        }
    }
private:
    TScopedLock *Lock;
};

class TLockable {
public:
    TScopedLock ScopedLock() {
        return TScopedLock(Mutex);
    }
private:
    std::mutex Mutex;
};

class TNestedScopedLock : public TNonCopyable {
    TScopedLock InnerLock;

public:
    TNestedScopedLock(TLockable &inner, TScopedLock &outer) {
        PORTO_ASSERT(outer);

        if (AllowHolderUnlock) {
            TScopedUnlock unlock(outer);
            InnerLock = inner.ScopedLock();
        }
    }
};
