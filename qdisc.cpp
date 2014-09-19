#include "qdisc.hpp"
#include "util/log.hpp"

TError TTclass::GetStat(ETclassStat stat, uint64_t &val) {
    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.GetStat(Handle, stat, val); });
}

uint32_t TTclass::GetParent() {
    if (ParentQdisc)
        return ParentQdisc->GetHandle();
    else
        return ParentTclass->Handle;
}

TError TTclass::Create(uint32_t prio, uint32_t rate, uint32_t ceil) {

    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.AddClass(GetParent(), Handle, prio, rate, ceil); });
}

TError TTclass::Remove() {
    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.RemoveClass(GetParent(), Handle); });
}

TError TQdisc::Create() {
    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.AddHTB(TcRootHandle(), Handle, DefClass); });
}

TError TQdisc::Remove() {
    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.RemoveHTB(TcRootHandle()); });
}

TError TFilter::Create() {
    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.AddCgroupFilter(Parent->GetHandle(), 1); });
}

TError TFilter::Remove() {
    return TNetlink::Exec([&](TNetlink &nl) {
        return nl.RemoveCgroupFilter(Parent->GetHandle(), 1); });
}
