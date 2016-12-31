#pragma once

#include <string>

#include "util/error.hpp"

/* Allows numeric user prepresentation */
TError FindUser(const std::string &user, uid_t &uid, gid_t &gid);
TError UserId(const std::string &user, uid_t &uid);
std::string UserName(uid_t uid);

TError FindGroups(const std::string &user, gid_t gid, std::vector<gid_t> &groups);

TError GroupId(const std::string &group, gid_t &gid);
std::string GroupName(gid_t gid);

TError InitCred();

constexpr uid_t RootUser = (uid_t)0;
constexpr gid_t RootGroup = (gid_t)0;

constexpr uid_t NoUser = (uid_t)-1;
constexpr gid_t NoGroup = (gid_t)-1;

extern gid_t PortoGroup;
extern gid_t PortoCtGroup;

struct TCred {
    uid_t Uid;
    gid_t Gid;
    std::vector<gid_t> Groups;

    TCred(uid_t uid, gid_t gid) : Uid(uid), Gid(gid) {}
    TCred() : Uid(NoUser), Gid(NoGroup) {}

    static TCred Current();

    TError Load(const std::string &user);
    TError LoadGroups(const std::string &user);

    TError Apply() const;

    std::string User() const {
        return UserName(Uid);
    }

    std::string Group() const {
        return GroupName(Gid);
    }

    bool IsRootUser() const { return Uid == RootUser; }

    bool IsMemberOf(gid_t group) const;

    std::string ToString() const {
        return User() + ":" + Group();
    }

    friend std::ostream& operator<<(std::ostream& os, const TCred& cred) {
        return os << cred.ToString();
    }
};

void InitCapabilities();

struct TCapabilities {
    uint64_t Permitted;

    TError Parse(const std::string &str);
    std::string Format() const;
    TError Apply(int mask) const;
    TError ApplyLimit() const;
    TError ApplyAmbient() const;
    TError ApplyEffective() const;
    TError Load(pid_t pid, int type);
    void Dump();
    friend std::ostream& operator<<(std::ostream& os, const TCapabilities &c) {
        return os << c.Format();
    }
    bool HasSetUidGid() const;
};

extern bool HasAmbientCapabilities;
extern TCapabilities NoCapabilities;
extern TCapabilities PortoInitCapabilities;
extern TCapabilities MemCgCapabilities;
extern TCapabilities PidNsCapabilities;
extern TCapabilities NetNsCapabilities;
extern TCapabilities AppModeCapabilities;
extern TCapabilities OsModeCapabilities;
extern TCapabilities SuidCapabilities;
extern TCapabilities AllCapabilities;
