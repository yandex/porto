#include "qdisc.hpp"
#include "util/log.hpp"

const std::string &TTclass::GetDevice() {
    if (ParentTclass)
        return ParentTclass->GetDevice();
    else
        return ParentQdisc->GetDevice();
}

TError TTclass::GetStat(ETclassStat stat, uint64_t &val) {
    TNetlink nl;

    TError error = nl.Open(GetDevice());
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

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    error = nl.AddClass(GetParent(), Handle, prio, rate, ceil);
    if (error)
        return error;

    return TError::Success();
}

TError TTclass::Remove() {
    TNetlink nl;

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    error = nl.RemoveClass(GetParent(), Handle);
    if (error)
        return error;

    return TError::Success();
}

TError TQdisc::Create() {
    TNetlink nl;

    TError error = nl.Open(Device);
    if (error)
        return error;

    error = nl.AddHTB(TcRootHandle(), Handle, DefClass);
    if (error)
        return error;

    return TError::Success();
}

TError TQdisc::Remove() {
    TNetlink nl;

    TError error = nl.Open(Device);
    if (error)
        return error;

    error = nl.RemoveHTB(TcRootHandle());
    if (error)
        return error;

    return TError::Success();
}

const std::string &TQdisc::GetDevice() {
    return Device;
}
