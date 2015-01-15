#include "util/string.hpp"
#include "util/log.hpp"
#include "util/cred.hpp"

extern "C" {
#include <grp.h>
#include <pwd.h>
}

std::string TUserEntry::GetName() {
    return Name;
}

int TUserEntry::GetId() {
    return Id;
}

TError TUser::Load() {
    struct passwd *p;

    if (Id >= 0) {
        p = getpwuid(Id);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }

        return TError(EError::InvalidValue, "Invalid uid: " + std::to_string(Id));
    }

    if (Name.length()) {
        p = getpwnam(Name.c_str());
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }

        int uid;
        TError error = StringToInt(Name, uid);
        if (error)
            return TError(EError::InvalidValue, "Invalid user: " + Name);

        p = getpwuid(uid);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }
    }

    return TError(EError::InvalidValue, "Invalid user");
}

TError TGroup::Load() {
    struct group *g;

    if (Id >= 0) {
        g = getgrgid(Id);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }

        return TError(EError::InvalidValue, "Invalid gid: " + std::to_string(Id));
    }

    if (Name.length()) {
        g = getgrnam(Name.c_str());
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }

        int uid;
        TError error = StringToInt(Name, uid);
        if (error)
            return TError(EError::InvalidValue, "Invalid group: " + Name);

        g = getgrgid(uid);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }
    }

    return TError(EError::InvalidValue, "Invalid group");
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

TError parseCred(TCred &cred, const std::string &user, const std::string &group) {
    TUser u(user);
    TError error = u.Load();
    if (error)
        return error;

    TGroup g(group);
    error = g.Load();
    if (error)
        return error;

    cred.Uid = u.GetId();
    cred.Gid = g.GetId();

    return TError::Success();
}
