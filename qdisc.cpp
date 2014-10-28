#include "qdisc.hpp"
#include "util/log.hpp"

bool TTclass::Exists() {
    if (!config().network().enabled())
        return false;

    TNlClass tclass(Link, GetParent(), Handle);
    return tclass.Exists();
}

TError TTclass::GetStat(ETclassStat stat, uint64_t &val) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    TNlClass tclass(Link, GetParent(), Handle);
    return tclass.GetStat(stat, val);
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

    TNlClass tclass(Link, GetParent(), Handle);
    return tclass.Create(prio, rate, ceil);
}

TError TTclass::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    TNlClass tclass(Link, GetParent(), Handle);
    return tclass.Remove();
}

TError TQdisc::Create() {
    if (!config().network().enabled())
        return TError::Success();

    TNlHtb qdisc(Link, TcRootHandle(), Handle);
    return qdisc.Create(DefClass);
}

TError TQdisc::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    TNlHtb qdisc(Link, TcRootHandle(), Handle);
    return qdisc.Remove();
}

bool TFilter::Exists() {
    if (!config().network().enabled())
        return false;

    TNlCgFilter filter(Link, Parent->GetHandle(), 1);
    return filter.Exists();
}

TError TFilter::Create() {
    if (!config().network().enabled())
        return TError::Success();

    TNlCgFilter filter(Link, Parent->GetHandle(), 1);
    return filter.Create();
}

TError TFilter::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    TNlCgFilter filter(Link, Parent->GetHandle(), 1);
    return filter.Remove();
}
