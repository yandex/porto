#include "qdisc.hpp"
#include "util/log.hpp"

TError TTclass::GetStat(ETclassStat stat, uint64_t &val) {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.GetStat(Handle, stat, val);
}

uint32_t TTclass::GetParent() {
    if (ParentQdisc)
        return ParentQdisc->GetHandle();
    else
        return ParentTclass->Handle;
}

TError TTclass::Create(uint32_t prio, uint32_t rate, uint32_t ceil) {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.AddClass(GetParent(), Handle, prio, rate, ceil);
}

TError TTclass::Remove() {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.RemoveClass(GetParent(), Handle);
}

TError TQdisc::Create() {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.AddHTB(TcRootHandle(), Handle, DefClass);
}

TError TQdisc::Remove() {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.RemoveHTB(TcRootHandle());
}

TError TFilter::Create() {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.AddCgroupFilter(Parent->GetHandle(), 1);
}

TError TFilter::Remove() {
    TNetlink nl;

    TError error = nl.Open();
    if (error)
        return error;

    return nl.RemoveCgroupFilter(Parent->GetHandle(), 1);
}
