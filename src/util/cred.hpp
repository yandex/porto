#pragma once

#include <string>

#include "util/error.hpp"

TError FindUser(const std::string &user, uid_t &uid, gid_t &gid);
TError FindGroups(const std::string &user, gid_t gid, std::vector<gid_t> &groups);

/* Allows numeric user-group prepresentation */
TError UserId(const std::string &user, uid_t &uid);
std::string UserName(uid_t uid);

TError GroupId(const std::string &group, gid_t &gid);
std::string GroupName(gid_t gid);

void InitCred();

extern gid_t GetPortoGroupId();

extern int LastCapability;

struct TCred {
    uid_t Uid;
    gid_t Gid;
    std::vector<gid_t> Groups;

    TCred(uid_t uid, gid_t gid) : Uid(uid), Gid(gid) {}
    TCred() : Uid(-1), Gid(-1) {}

    static TCred Current();

    TError Load(const std::string &user);
    TError LoadGroups(const std::string &user);

    std::string User() const {
        return UserName(Uid);
    }

    std::string Group() const {
        return GroupName(Gid);
    }

    bool IsPortoUser() const;
    bool IsRootUser() const { return Uid == 0; }

    bool IsMemberOf(gid_t group) const;
    bool IsPermitted(const TCred &requirement) const;

    friend std::ostream& operator<<(std::ostream& os, const TCred& cred) {
        return os << cred.User() << ":" << cred.Group();
    }
};
