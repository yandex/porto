#pragma once

#include <mutex>
#include "common.hpp"

typedef std::unique_lock<std::mutex> TScopedLock;

class TScopedUnlock : public TNonCopyable {
public:
    TScopedUnlock(TScopedLock &lock);
    ~TScopedUnlock();
private:
    TScopedLock *Lock;
};

class TLockable {
public:
    TScopedLock ScopedLock();
    TScopedLock TryScopedLock();
private:
    std::mutex Mutex;
};

class TNestedScopedLock {
    TScopedLock InnerLock;

    TNestedScopedLock(TNestedScopedLock const&) = delete;
    TNestedScopedLock& operator= (TNestedScopedLock const&) = delete;

public:
    TNestedScopedLock();
    TNestedScopedLock(TNestedScopedLock &&src);
    TNestedScopedLock& operator=(TNestedScopedLock &&src);
    TNestedScopedLock(TLockable &inner, TScopedLock &outer);
    TNestedScopedLock(TLockable &inner, TScopedLock &outer, std::try_to_lock_t t);
    bool IsLocked();
};
