#include "util/cred.hpp"
#include "util/log.hpp"
#include "config.hpp"
#include "common.hpp"

extern "C" {
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/securebits.h>
}

gid_t PortoGroup;
gid_t PortoCtGroup;

static size_t PwdBufSize = sysconf(_SC_GETPW_R_SIZE_MAX) > 0 ?
                           sysconf(_SC_GETPW_R_SIZE_MAX) : 16384;

TError FindUser(const std::string &user, uid_t &uid, gid_t &gid) {
    struct passwd pwd, *ptr;
    char buf[PwdBufSize];
    int id, err;

    if (isdigit(user[0]) && !StringToInt(user, id) && id >= 0)
        err = getpwuid_r(id, &pwd, buf, PwdBufSize, &ptr);
    else
        err = getpwnam_r(user.c_str(), &pwd, buf, PwdBufSize, &ptr);

    if (err || !ptr)
        return TError(EError::InvalidValue, errno, "Cannot find user: " + user);

    uid = pwd.pw_uid;
    gid = pwd.pw_gid;
    return TError::Success();
}

TError FindGroups(const std::string &user, gid_t gid, std::vector<gid_t> &groups) {
    int ngroups = 32;

    for (int retry = 0; retry < 3; retry++) {
        groups.resize(ngroups);
        if (getgrouplist(user.c_str(), gid, groups.data(), &ngroups) >= 0) {
            groups.resize(ngroups);
            return TError::Success();
        }
    }

    return TError(EError::Unknown, "Cannot list groups for " + user);
}

TError UserId(const std::string &user, uid_t &uid) {
    struct passwd pwd, *ptr;
    char buf[PwdBufSize];
    int id;

    if (isdigit(user[0]) && !StringToInt(user, id) && id >= 0) {
        uid = id;
        return TError::Success();
    }

    if (getpwnam_r(user.c_str(), &pwd, buf, PwdBufSize, &ptr) || !ptr)
        return TError(EError::InvalidValue, errno, "Cannot find user: " + user);

    uid = pwd.pw_uid;
    return TError::Success();
}

std::string UserName(uid_t uid) {
    struct passwd pwd, *ptr;
    char buf[PwdBufSize];

    if (getpwuid_r(uid, &pwd, buf, PwdBufSize, &ptr) || !ptr)
        return std::to_string(uid);

    return std::string(pwd.pw_name);
}

static size_t GrpBufSize = sysconf(_SC_GETGR_R_SIZE_MAX) > 0 ?
                           sysconf(_SC_GETGR_R_SIZE_MAX) : 16384;

TError GroupId(const std::string &group, gid_t &gid) {
    struct group grp, *ptr;
    char buf[GrpBufSize];
    int id;

    if (isdigit(group[0]) && !StringToInt(group, id) && id >= 0) {
        gid = id;
        return TError::Success();
    }

    if (getgrnam_r(group.c_str(), &grp, buf, GrpBufSize, &ptr) || !ptr)
        return TError(EError::InvalidValue, errno, "Cannot find group: " + group);

    gid = grp.gr_gid;
    return TError::Success();
}

std::string GroupName(gid_t gid) {
    struct group grp, *ptr;
    char buf[GrpBufSize];

    if (getgrgid_r(gid, &grp, buf, GrpBufSize, &ptr) || !ptr)
        return std::to_string(gid);

    return std::string(grp.gr_name);
}

TCred TCred::Current() {
    TCred cred(geteuid(), getegid());

    cred.Groups.resize(getgroups(0, nullptr));
    if (getgroups(cred.Groups.size(), cred.Groups.data()) < 0) {
        L_ERR() << "Cannot get supplementary groups for " << cred.Uid << std::endl;
        cred.Groups.resize(1);
        cred.Groups[0] = cred.Gid;
    }

    return cred;
}

TError TCred::LoadGroups(const std::string &user) {
    if (FindGroups(user, Gid, Groups)) {
        L_ERR() << "Cannot load groups for " << user << std::endl;
        Groups.resize(1);
        Groups[0] = Gid;
    }
    return TError::Success();
}

TError TCred::Load(const std::string &user) {
    TError error = FindUser(user, Uid, Gid);
    if (!error)
        error = LoadGroups(user);
    return error;
}

bool TCred::IsMemberOf(gid_t group) const {
    if (group == NoGroup)
        return false;

    if (Gid == group)
        return true;

    for (auto id: Groups) {
        if (id == group)
            return true;
    }

    return false;
}

TError TCred::Apply() const {
    if (prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS | SECBIT_NO_SETUID_FIXUP, 0, 0, 0) < 0)
        return TError(EError::Unknown, errno, "prctl(PR_SET_KEEPCAPS, 1)");

    if (setgid(Gid) < 0)
        return TError(EError::Unknown, errno, "setgid()");

    if (setgroups(Groups.size(), Groups.data()) < 0)
        return TError(EError::Unknown, errno, "setgroups()");

    if (setuid(Uid) < 0)
        return TError(EError::Unknown, errno, "setuid()");

    if (prctl(PR_SET_SECUREBITS, 0, 0, 0, 0) < 0)
        return TError(EError::Unknown, errno, "prctl(PR_SET_KEEPCAPS, 0)");

    return TError::Success();
}

TError InitCred() {
    TError error;

    error = GroupId(PORTO_GROUP_NAME, PortoGroup);
    if (error) {
        L_ERR() << "Cannot find group " << PORTO_GROUP_NAME << ": " << error << std::endl;
        return error;
    }

    error = GroupId(PORTO_CT_GROUP_NAME, PortoCtGroup);
    if (error) {
        L_ERR() << "Cannot find group " << PORTO_CT_GROUP_NAME << ": " << error << std::endl;
        return error;
    }

    return error;
}

#ifndef CAP_BLOCK_SUSPEND
#define CAP_BLOCK_SUSPEND 36
#endif

#ifndef CAP_AUDIT_READ
#define CAP_AUDIT_READ 37
#endif

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT              47
# define PR_CAP_AMBIENT_IS_SET      1
# define PR_CAP_AMBIENT_RAISE       2
# define PR_CAP_AMBIENT_LOWER       3
# define PR_CAP_AMBIENT_CLEAR_ALL   4
#endif

static const TFlagsNames CapNames = {
    { BIT(CAP_CHOWN),            "CHOWN" },
    { BIT(CAP_DAC_OVERRIDE),     "DAC_OVERRIDE" },
    { BIT(CAP_DAC_READ_SEARCH),  "DAC_READ_SEARCH" },
    { BIT(CAP_FOWNER),           "FOWNER" },
    { BIT(CAP_FSETID),           "FSETID" },
    { BIT(CAP_KILL),             "KILL" },
    { BIT(CAP_SETGID),           "SETGID" },
    { BIT(CAP_SETUID),           "SETUID" },
    { BIT(CAP_SETPCAP),          "SETPCAP" },
    { BIT(CAP_LINUX_IMMUTABLE),  "LINUX_IMMUTABLE" },
    { BIT(CAP_NET_BIND_SERVICE), "NET_BIND_SERVICE" },
    { BIT(CAP_NET_BROADCAST),    "NET_BROADCAST" },
    { BIT(CAP_NET_ADMIN),        "NET_ADMIN" },
    { BIT(CAP_NET_RAW),          "NET_RAW" },
    { BIT(CAP_IPC_LOCK),         "IPC_LOCK" },
    { BIT(CAP_IPC_OWNER),        "IPC_OWNER" },
    { BIT(CAP_SYS_MODULE),       "SYS_MODULE" },
    { BIT(CAP_SYS_RAWIO),        "SYS_RAWIO" },
    { BIT(CAP_SYS_CHROOT),       "SYS_CHROOT" },
    { BIT(CAP_SYS_PTRACE),       "SYS_PTRACE" },
    { BIT(CAP_SYS_PACCT),        "SYS_PACCT" },
    { BIT(CAP_SYS_ADMIN),        "SYS_ADMIN" },
    { BIT(CAP_SYS_BOOT),         "SYS_BOOT" },
    { BIT(CAP_SYS_NICE),         "SYS_NICE" },
    { BIT(CAP_SYS_RESOURCE),     "SYS_RESOURCE" },
    { BIT(CAP_SYS_TIME),         "SYS_TIME" },
    { BIT(CAP_SYS_TTY_CONFIG),   "SYS_TTY_CONFIG" },
    { BIT(CAP_MKNOD),            "MKNOD" },
    { BIT(CAP_LEASE),            "LEASE" },
    { BIT(CAP_AUDIT_WRITE),      "AUDIT_WRITE" },
    { BIT(CAP_AUDIT_CONTROL),    "AUDIT_CONTROL" },
    { BIT(CAP_SETFCAP),          "SETFCAP" },
    { BIT(CAP_MAC_OVERRIDE),     "MAC_OVERRIDE" },
    { BIT(CAP_MAC_ADMIN),        "MAC_ADMIN" },
    { BIT(CAP_SYSLOG),           "SYSLOG" },
    { BIT(CAP_WAKE_ALARM),       "WAKE_ALARM" },
    { BIT(CAP_BLOCK_SUSPEND),    "BLOCK_SUSPEND" },
    { BIT(CAP_AUDIT_READ),       "AUDIT_READ" },
};

static int LastCapability;

bool HasAmbientCapabilities = false;

std::string TCapabilities::Format() const {
    return StringFormatFlags(Permitted, CapNames, "; ");
}

TError TCapabilities::Parse(const std::string &string) {
    return StringParseFlags(string, CapNames, Permitted, ';');
}

TError TCapabilities::Load(pid_t pid, int type) {
    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = getpid(),
    };
    struct __user_cap_data_struct data[2];

    if (syscall(SYS_capget, &header, data) < 0)
        return TError(EError::Unknown, errno, "capget " + Format());

    switch (type) {
        case 0: Permitted = data[0].effective | (uint64_t)data[1].effective << 32; break;
        case 1: Permitted = data[0].permitted | (uint64_t)data[1].permitted << 32; break;
        case 2: Permitted = data[0].inheritable | (uint64_t)data[1].inheritable << 32; break;
    }

    return TError::Success();
}

void TCapabilities::Dump() {
    Load(0, 0);
    L() << "Effective: " << Format() << std::endl;
    Load(0, 1);
    L() << "Permitted: " << Format() << std::endl;
    Load(0, 2);
    L() << "Inheritable: " << Format() << std::endl;
}

TError TCapabilities::Apply(int mask) const {
    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = getpid(),
    };
    struct __user_cap_data_struct data[2];

    if (mask != 7 && syscall(SYS_capget, &header, data) < 0)
        return TError(EError::Unknown, errno, "capget");

    if (mask & 1) {
        data[0].effective = Permitted;
        data[1].effective = Permitted >> 32;
    }
    if (mask & 2) {
        data[0].permitted = Permitted;
        data[1].permitted = Permitted >> 32;
    }
    if (mask & 4) {
        data[0].inheritable = Permitted;
        data[1].inheritable = Permitted >> 32;
    }

    if (syscall(SYS_capset, &header, data) < 0)
        return TError(EError::Unknown, errno, "capset " + Format());

    return TError::Success();
}

TError TCapabilities::ApplyLimit() const {
    for (int cap = 0; cap <= LastCapability; cap++) {
        if (!(Permitted & BIT(cap)) && cap != CAP_SETPCAP &&
                prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0)
            return TError(EError::Unknown, errno,
                    "prctl(PR_CAPBSET_DROP, " + std::to_string(cap) + ")");

    }

    if (!(Permitted & BIT(CAP_SETPCAP)) &&
            prctl(PR_CAPBSET_DROP, CAP_SETPCAP, 0, 0, 0) < 0)
        return TError(EError::Unknown, errno, "prctl(PR_CAPBSET_DROP, CAP_SETPCAP)");

    return TError::Success();
}

TError TCapabilities::ApplyAmbient() const {
    if (!HasAmbientCapabilities)
        return TError::Success();

    TError error = Apply(4);
    if (error)
        return error;

    for (int cap = 0; cap <= LastCapability; cap++) {
        if (Permitted & BIT(cap)) {
            if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0)) {
                return TError(EError::Unknown, errno,
                        "prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE)");
            }
        } else {
            if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, cap, 0, 0))
                return TError(EError::Unknown, errno,
                        "prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER)");
        }
    }

    return TError::Success();
}

TError TCapabilities::ApplyEffective() const {
    return Apply(1);
}

TCapabilities NoCapabilities;
TCapabilities PortoInitCapabilities;
TCapabilities MemCgCapabilities;
TCapabilities PidNsCapabilities;
TCapabilities NetNsCapabilities;
TCapabilities AppModeCapabilities;
TCapabilities OsModeCapabilities;
TCapabilities SuidCapabilities;
TCapabilities AllCapabilities;

void InitCapabilities() {
    if (TPath("/proc/sys/kernel/cap_last_cap").ReadInt(LastCapability)) {
        L_WRN() << "Can't read /proc/sys/kernel/cap_last_cap" << std::endl;
        LastCapability = CAP_AUDIT_READ;
    }

    HasAmbientCapabilities = prctl(PR_CAP_AMBIENT,
                                   PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == 0;

    NoCapabilities.Permitted = 0;

    PortoInitCapabilities.Permitted = BIT(CAP_KILL);

    AllCapabilities.Permitted = BIT(LastCapability + 1) - 1;

    MemCgCapabilities.Permitted =
        BIT(CAP_IPC_LOCK);

    PidNsCapabilities.Permitted =
        BIT(CAP_KILL) |
        BIT(CAP_SYS_PTRACE);

    NetNsCapabilities.Permitted =
        BIT(CAP_NET_BIND_SERVICE) |
        BIT(CAP_NET_ADMIN) |
        BIT(CAP_NET_RAW);

    AppModeCapabilities.Permitted =
        MemCgCapabilities.Permitted |
        PidNsCapabilities.Permitted |
        NetNsCapabilities.Permitted;

    OsModeCapabilities.Permitted =
        AppModeCapabilities.Permitted |
        BIT(CAP_CHOWN) |
        BIT(CAP_DAC_OVERRIDE) |
        BIT(CAP_FOWNER) |
        BIT(CAP_FSETID) |
        BIT(CAP_SETGID) |
        BIT(CAP_SETUID) |
        BIT(CAP_SYS_CHROOT) |
        BIT(CAP_MKNOD) |
        BIT(CAP_AUDIT_WRITE);

    SuidCapabilities.Permitted =
        OsModeCapabilities.Permitted |
        BIT(CAP_SETPCAP) |
        BIT(CAP_SETFCAP) |
        BIT(CAP_LINUX_IMMUTABLE) |
        BIT(CAP_SYS_ADMIN) |
        BIT(CAP_SYS_NICE) |
        BIT(CAP_SYS_RESOURCE);
}
