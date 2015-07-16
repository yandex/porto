#pragma once

#include <mutex>
#include "common.hpp"

constexpr bool AllowHolderUnlock = true;

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
};
