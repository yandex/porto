#include "qdisc.hpp"
#include "util/log.hpp"

bool TTclass::Exists() {
    if (!config().network().enabled())
        return false;

    return TNetlink::Exec([&](TNetlink &nl) {
        if (nl.ClassExists(Handle))
            return TError::Success();
        else
            return TError(EError::Unknown, "");
    }) == TError::Success();
}

TError TTclass::GetStat(ETclassStat stat, uint64_t &val) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.GetStat(Handle, stat, val); });
}

uint32_t TTclass::GetParent() {
    if (!config().network().enabled())
        return 0;

    if (ParentQdisc)
        return ParentQdisc->GetHandle();
    else
        return ParentTclass->Handle;
}

TError TTclass::Create(uint32_t prio, uint32_t rate, uint32_t ceil) {
    if (!config().network().enabled())
        return TError::Success();

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.AddClass(GetParent(), Handle, prio, rate, ceil); });
}

TError TTclass::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.RemoveClass(GetParent(), Handle); });
}

TError TQdisc::Create() {
    if (!config().network().enabled())
        return TError::Success();

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.AddHTB(TcRootHandle(), Handle, DefClass); });
}

TError TQdisc::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.RemoveHTB(TcRootHandle()); });
}

bool TFilter::Exists() {
    if (!config().network().enabled())
        return false;

    return TNetlink::Exec([&](TNetlink &nl) {
        if (nl.CgroupFilterExists(Parent->GetHandle(), 1))
            return TError::Success();
        else
            return TError(EError::Unknown, "");
    }) == TError::Success();
}

TError TFilter::Create() {
    if (!config().network().enabled())
        return TError::Success();

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.AddCgroupFilter(Parent->GetHandle(), 1); });
}

TError TFilter::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.RemoveCgroupFilter(Parent->GetHandle(), 1); });
}
