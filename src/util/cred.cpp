#include "util/cred.hpp"
#include "util/log.hpp"
#include "config.hpp"
#include "common.hpp"

extern "C" {
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
}

static gid_t PortoGroup;

static std::vector<uid_t> RestrictedRootUid;

static size_t PwdBufSize = sysconf(_SC_GETPW_R_SIZE_MAX) > 0 ?
                           sysconf(_SC_GETPW_R_SIZE_MAX) : 16384;

TError FindUser(const std::string &user, uid_t &uid, gid_t &gid) {
    struct passwd pwd, *ptr;
    char buf[PwdBufSize];

    if (getpwnam_r(user.c_str(), &pwd, buf, PwdBufSize, &ptr) || !ptr)
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

//FIXME should be euqal to IsPortoUser
bool TCred::IsRestrictedRootUser() const {

    for (auto id: RestrictedRootUid) {
        if (Uid == id)
            return true;
    }

    return IsRootUser();
}

bool TCred::IsPortoUser() const {
    return IsRootUser() || IsMemberOf(PortoGroup);
}

/* Returns true for priveleged or if uid/gid intersects */
bool TCred::IsPermitted(const TCred &requirement) const {

    if (Uid == requirement.Uid)
        return true;

    if (IsRootUser())
        return true;

    if (IsMemberOf(requirement.Gid))
        return true;

    for (auto gid: requirement.Groups)
        if (IsMemberOf(gid))
            return true;

    return false;
}

bool TCred::IsMemberOf(gid_t group) const {

    for (auto id: Groups) {
        if (id == group)
            return true;
    }

    return Gid == group;
}

void InitCred() {
    TError error;

    error = GroupId(PORTO_GROUP_NAME, PortoGroup);
    if (error)
        L_WRN() << "Cannot find group porto: " << error << std::endl;

    for (auto &user: config().privileges().restricted_root_user()) {
        uid_t uid;
        error = UserId(user, uid);
        if (error)
            L_WRN() << "Can't add privileged user " << user << " : "  << error << std::endl;
        else
            RestrictedRootUid.push_back(uid);
    }
}
