#include "pwd.hpp"
#include "util/string.hpp"

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

    if (Id) {
        p = getpwuid(Id);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        }

        return TError(EError::InvalidValue, EINVAL, "Invalid uid");
    }

    p = getpwnam(Name.c_str());
    if (p) {
        Id = p->pw_uid;
        Name = p->pw_name;
        return TError::Success();
    }

    int uid;
    TError error = StringToInt(Name, uid);
    if (error)
        return error;

    p = getpwuid(uid);
    if (p) {
        Id = p->pw_uid;
        Name = p->pw_name;
        return TError::Success();
    }

    return TError(EError::InvalidValue, EINVAL, "Invalid user");
}

TError TGroup::Load() {
    struct group *g;

    if (Id) {
        g = getgrgid(Id);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        }

        return TError(EError::InvalidValue, EINVAL, "Invalid uid");
    }

    g = getgrnam(Name.c_str());
    if (g) {
        Id = g->gr_gid;
        Name = g->gr_name;
        return TError::Success();
    }

    int uid;
    TError error = StringToInt(Name, uid);
    if (error)
        return error;

    g = getgrgid(uid);
    if (g) {
        Id = g->gr_gid;
        Name = g->gr_name;
        return TError::Success();
    }

    return TError(EError::InvalidValue, EINVAL, "Invalid group");
}
