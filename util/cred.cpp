#include "util/string.hpp"
#include "util/log.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

#include "config.hpp"

extern "C" {
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
}

std::string TUserEntry::GetName() {
    return Name;
}

int TUserEntry::GetId() {
    return Id;
}

static size_t GetPwSize() {
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize < 0)
        return 16384;
    return bufsize;
}

TError TUser::Load() {
    struct passwd pwd, *p;
    TScopedMem buf(GetPwSize());

    if (Id >= 0) {
        getpwuid_r(Id, &pwd, (char *)buf.GetData(), buf.GetSize(), &p);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }

        return TError(EError::InvalidValue, "Invalid uid: " + std::to_string(Id));
    }

    if (Name.length()) {
        getpwnam_r(Name.c_str(), &pwd, (char *)buf.GetData(), buf.GetSize(), &p);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }

        int uid;
        TError error = StringToInt(Name, uid);
        if (error)
            return TError(EError::InvalidValue, "Invalid user: " + Name);

        getpwuid_r(Id, &pwd, (char *)buf.GetData(), buf.GetSize(), &p);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }
    }

    return TError(EError::InvalidValue, "Invalid user");
}

TError TGroup::Load() {
    struct group grp, *g;
    TScopedMem buf(GetPwSize());

    if (Id >= 0) {
        getgrgid_r(Id, &grp, (char *)buf.GetData(), buf.GetSize(), &g);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }

        return TError(EError::InvalidValue, "Invalid gid: " + std::to_string(Id));
    }

    if (Name.length()) {
        getgrnam_r(Name.c_str(), &grp, (char *)buf.GetData(), buf.GetSize(), &g);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }

        int uid;
        TError error = StringToInt(Name, uid);
        if (error)
            return TError(EError::InvalidValue, "Invalid group: " + Name);

        getgrgid_r(Id, &grp, (char *)buf.GetData(), buf.GetSize(), &g);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }
    }

    return TError(EError::InvalidValue, "Invalid group");
}

bool TCred::IsPrivileged() const {
    if (IsRoot())
        return true;

    return CredConf.PrivilegedUser(*this);
}

std::string TCred::UserAsString() const {
    TUser u(Uid);

    if (u.Load())
        return std::to_string(Uid);
    else
        return u.GetName();
}

std::string TCred::GroupAsString() const {
    TGroup g(Gid);

    if (g.Load())
        return std::to_string(Gid);
    else
        return g.GetName();
};

TError TCred::Parse(const std::string &user, const std::string &group) {
    TUser u(user);
    TError error = u.Load();
    if (error)
        return error;

    TGroup g(group);
    error = g.Load();
    if (error)
        return error;

    Uid = u.GetId();
    Gid = g.GetId();

    return TError::Success();
}

static void ParseUserConf(const ::google::protobuf::RepeatedPtrField<std::string> &source,
                          std::set<int> &target) {
    for (auto &val : source) {
        TUser u(val);
        TError error = u.Load();
        if (error) {
            L_WRN() << "Can't add privileged user: " << error << std::endl;
            continue;
        }

        target.insert(u.GetId());
    }
}

static void ParseGroupConf(const ::google::protobuf::RepeatedPtrField<std::string> &source,
                          std::set<int> &target) {
    for (auto &val : source) {
        TGroup g(val);
        TError error = g.Load();
        if (error) {
            L_WRN() << "Can't add privileged group: " << error << std::endl;
            continue;
        }

        target.insert(g.GetId());
    }
}

void TCredConf::Load() {
    ParseUserConf(config().privileges().root_user(), PrivilegedUid);
    ParseGroupConf(config().privileges().root_group(), PrivilegedGid);

    ParseUserConf(config().privileges().restricted_root_user(), RestrictedRootUid);
    ParseGroupConf(config().privileges().restricted_root_group(), RestrictedRootGid);
}

bool TCredConf::PrivilegedUser(const TCred &cred) {
    if (PrivilegedUid.find(cred.Uid) != PrivilegedUid.end())
        return true;

    if (PrivilegedGid.find(cred.Gid) != PrivilegedGid.end())
        return true;

    return false;
}

bool TCredConf::RestrictedUser(const TCred &cred) {
    if (RestrictedRootUid.find(cred.Uid) != RestrictedRootUid.end())
        return true;

    if (RestrictedRootGid.find(cred.Gid) != RestrictedRootGid.end())
        return true;

    return false;
}

TCredConf CredConf;
