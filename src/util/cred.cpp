#include <algorithm>

#include "util/string.hpp"
#include "util/log.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "util/file.hpp"
#include "util/string.hpp"

#include "config.hpp"

extern "C" {
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
}

TUserEntry::TUserEntry(const std::string &name) :
    Name(!name.empty() && StringOnlyDigits(name) ? "" : name),
    Id(!name.empty() && StringOnlyDigits(name) ? stoi(name) : -1) {}

std::string TUserEntry::GetName() const {
    return Name;
}

int TUserEntry::GetId() const {
    return Id;
}

TError TUserEntry::LoadFromFile(const TPath &path) {
    std::vector<std::string> lines;

    TFile f(path);
    TError error = f.AsLines(lines);
    for (auto line : lines) {
        std::vector<std::string> tok;
        TError error = SplitString(line, ':', tok);
        if (error)
            return error;
        if (tok.size() < 3)
            continue;

        int entryId;
        error = StringToInt(tok[2], entryId);
        if (error)
            continue;

        if (Id >= 0) {
            if (entryId == Id) {
                Name = tok[0];
                return TError::Success();
            }
        } else {
            if (tok[0] == Name) {
                Id = entryId;
                return TError::Success();
            }
        }
    }

    return TError(EError::InvalidValue, "Entry not found in " + path.ToString());
}

static size_t GetPwSize() {
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize < 32768)
        return 32768;
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
        } else if (errno == ENOMEM || errno == ERANGE) {
            L_WRN() << "Not enough space in buffer for credentials" << std::endl;
        }
        return TError::Success();
    }

    if (Name.length()) {
        getpwnam_r(Name.c_str(), &pwd, (char *)buf.GetData(), buf.GetSize(), &p);
        if (p) {
            Id = p->pw_uid;
            Name = p->pw_name;
            return TError::Success();
        } else if (errno == ENOMEM || errno == ERANGE) {
            L_WRN() << "Not enough space in buffer for credentials" << std::endl;
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
        } else if (errno == ENOMEM || errno == ERANGE) {
            L_WRN() << "Not enough space in buffer for credentials" << std::endl;
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
        } else if (errno == ENOMEM || errno == ERANGE) {
            L_WRN() << "Not enough space in buffer for credentials" << std::endl;
        }

        return TError::Success();
    }

    if (Name.length()) {
        getgrnam_r(Name.c_str(), &grp, (char *)buf.GetData(), buf.GetSize(), &g);
        if (g) {
            Id = g->gr_gid;
            Name = g->gr_name;
            return TError::Success();
        } else if (errno == ENOMEM || errno == ERANGE) {
            L_WRN() << "Not enough space in buffer for credentials" << std::endl;
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
        } else if (errno == ENOMEM || errno == ERANGE) {
            L_WRN() << "Not enough space in buffer for credentials" << std::endl;
        }
    }

    return TError(EError::InvalidValue, "Invalid group");
}

TError TCred::LoadGroups(std::string user) {
    int ngroups = 0;

    if (user == "") {
        TUser u(Uid);
        TError error = u.Load();
        if (error)
            return error;
        user = u.GetName();
    }

    (void)getgrouplist(user.c_str(), Gid, nullptr, &ngroups);
    Groups.resize(ngroups);
    if (getgrouplist(user.c_str(), Gid, Groups.data(), &ngroups) < 0)
        return TError(EError::Unknown, errno, "Can't get supplementary groups");
    return TError::Success();
}

/* FIXME should die */
bool TCred::IsRootUser() const {
    return Uid == 0 || Gid == 0;
}

bool TCred::IsPrivileged() const {
    if (IsRootUser())
        return true;

    return CredConf.PrivilegedUser(*this);
}

/* Returns true for priveleged or if uid/gid intersects */
bool TCred::IsPermitted(const TCred &requirement) const {

    if (Uid == requirement.Uid)
        return true;

    if (IsPrivileged())
        return true;

    if (IsMemberOf(requirement.Gid))
        return true;

    for (auto gid: requirement.Groups)
        if (IsMemberOf(gid))
            return true;

    return false;
}

bool TCred::IsMemberOf(gid_t gid) const {
    return Gid == gid || std::find(Groups.begin(), Groups.end(), gid) != Groups.end();
}

std::string TCred::UserAsString() const {
    TUser u(Uid);

    (void)u.Load();
    if (u.GetName().empty())
        return std::to_string(Uid);
    else
        return u.GetName();
}

std::string TCred::GroupAsString() const {
    TGroup g(Gid);

    (void)g.Load();
    if (g.GetName().empty())
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

    TGroup porto("porto");
    TError error = porto.Load();
    if (error)
            L_WRN() << "Cannot load group porto: " << error << std::endl;
    else
        PortoGid = porto.GetId();
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
