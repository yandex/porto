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

static gid_t PortoGid;

static std::vector<uid_t> PrivilegedUid = { 0, };
static std::vector<gid_t> PrivilegedGid = { 0, };

static std::vector<uid_t> RestrictedRootUid;
static std::vector<gid_t> RestrictedRootGid;

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

/* FIXME should die */
bool TCred::IsRootUser() const {
    return Uid == 0 || Gid == 0;
}

bool TCred::IsPrivilegedUser() const {
    if (std::find(PrivilegedUid.begin(), PrivilegedUid.end(), Uid) != PrivilegedUid.end() ||
        std::find(PrivilegedGid.begin(), PrivilegedGid.end(), Gid) != PrivilegedGid.end())
        return true;

    return IsRootUser();
}

bool TCred::IsRestrictedRootUser() const {
    if (std::find(RestrictedRootUid.begin(), RestrictedRootUid.end(), Uid) != RestrictedRootUid.end() || 
        std::find(RestrictedRootGid.begin(), RestrictedRootGid.end(), Gid) != RestrictedRootGid.end())
        return true;

    return IsPrivilegedUser();
}

bool TCred::IsPortoUser() const {
    if (IsMemberOf(PortoGid))
        return true;

    return IsPrivilegedUser();
}

/* Returns true for priveleged or if uid/gid intersects */
bool TCred::IsPermitted(const TCred &requirement) const {

    if (Uid == requirement.Uid)
        return true;

    if (IsPrivilegedUser())
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

static void ParseUserConf(const ::google::protobuf::RepeatedPtrField<std::string> &source,
                          std::vector<uid_t> &target) {
    for (auto &user : source) {
        uid_t uid;
        TError error = UserId(user, uid);
        if (error)
            L_WRN() << "Can't add privileged user " << user << " : "  << error << std::endl;
        else
            target.push_back(uid);
    }
}

static void ParseGroupConf(const ::google::protobuf::RepeatedPtrField<std::string> &source,
                          std::vector<gid_t> &target) {
    for (auto &group : source) {
        gid_t gid;
        TError error = GroupId(group, gid);
        if (error)
            L_WRN() << "Can't add privileged group " << group << " : " << error << std::endl;
        else
            target.push_back(gid);
    }
}

void InitCred() {
    ParseUserConf(config().privileges().root_user(), PrivilegedUid);
    ParseGroupConf(config().privileges().root_group(), PrivilegedGid);

    ParseUserConf(config().privileges().restricted_root_user(), RestrictedRootUid);
    ParseGroupConf(config().privileges().restricted_root_group(), RestrictedRootGid);

    TError error = GroupId("porto", PortoGid);
    if (error)
        L_WRN() << "Cannot find group porto: " << error << std::endl;
}
