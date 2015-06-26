#pragma once

typedef std::unique_lock<std::mutex> TScopedLock;

class TScopedUnlock : public TNonCopyable {
public:
    TScopedUnlock(TScopedLock &lock) {
        Lock = &lock;
        Lock->unlock();
    }
    ~TScopedUnlock() {
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
        TScopedUnlock unlock(lock);
        return TScopedLock(Mutex);
    }
private:
    std::mutex Mutex;
};
