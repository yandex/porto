#include "locks.hpp"
#include "config.hpp"
#include "util/log.hpp"

//FIXME KILL THIS CRAP

TScopedUnlock::TScopedUnlock(TScopedLock &lock) {
    PORTO_ASSERT(lock.owns_lock());
    Lock = &lock;
    Lock->unlock();
}

TScopedUnlock::~TScopedUnlock() {
    PORTO_ASSERT(!Lock->owns_lock());
    Lock->lock();
}

TScopedLock TLockable::ScopedLock() {
    return TScopedLock(Mutex);
}

TScopedLock TLockable::TryScopedLock() {
    return TScopedLock(Mutex, std::try_to_lock);
}

TNestedScopedLock::TNestedScopedLock() {}

TNestedScopedLock::TNestedScopedLock(TNestedScopedLock &&src) : InnerLock(std::move(src.InnerLock)) {}
TNestedScopedLock& TNestedScopedLock::operator=(TNestedScopedLock &&src) {
    InnerLock = std::move(src.InnerLock);
    return *this;
}

TNestedScopedLock::TNestedScopedLock(TLockable &inner, TScopedLock &outer) {
    PORTO_ASSERT(outer.owns_lock());

    TScopedUnlock unlock(outer);
    InnerLock = inner.ScopedLock();
}

TNestedScopedLock::TNestedScopedLock(TLockable &inner, TScopedLock &outer, std::try_to_lock_t t) {
    PORTO_ASSERT(outer.owns_lock());

    TScopedUnlock unlock(outer);
    InnerLock = inner.TryScopedLock();
}

bool TNestedScopedLock::IsLocked() {
    return InnerLock.owns_lock();
}
