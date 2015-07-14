#include "locks.hpp"
#include "util/log.hpp"

TScopedUnlock::TScopedUnlock(TScopedLock &lock) {
    if (AllowHolderUnlock) {
        PORTO_ASSERT(lock.owns_lock());
        Lock = &lock;
        Lock->unlock();
    }
}

TScopedUnlock::~TScopedUnlock() {
    if (AllowHolderUnlock) {
        PORTO_ASSERT(!Lock->owns_lock());
        Lock->lock();
    }
}

TScopedLock TLockable::ScopedLock() {
    return TScopedLock(Mutex);
}

TNestedScopedLock::TNestedScopedLock() {}

TNestedScopedLock::TNestedScopedLock(TNestedScopedLock &&src) : InnerLock(std::move(src.InnerLock)) {}
TNestedScopedLock& TNestedScopedLock::operator=(TNestedScopedLock &&src) {
    InnerLock = std::move(src.InnerLock);
    return *this;
}

TNestedScopedLock::TNestedScopedLock(TLockable &inner, TScopedLock &outer) {
    PORTO_ASSERT(outer.owns_lock());

    if (AllowHolderUnlock) {
        TScopedUnlock unlock(outer);
        InnerLock = inner.ScopedLock();
    }
}
