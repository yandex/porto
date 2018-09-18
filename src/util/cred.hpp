#pragma once

#include <string>

#include "util/error.hpp"

/* Allows numeric user prepresentation */
TError FindUser(const TString &user, uid_t &uid, gid_t &gid);
TError UserId(const TString &user, uid_t &uid);
TString UserName(uid_t uid);

TError FindGroups(const TString &user, gid_t gid, std::vector<gid_t> &groups);

TError GroupId(const TString &group, gid_t &gid);
TString GroupName(gid_t gid);

void InitPortoGroups();

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

    void Enter() const;
    void Leave() const;

    TError Init(const TString &user);
    TError InitGroups(const TString &user);

    TError Apply() const;

    TString User() const {
        return UserName(Uid);
    }

    TString Group() const {
        return GroupName(Gid);
    }

    bool IsRootUser() const { return Uid == RootUser; }

    bool IsUnknown() const { return Uid == NoUser && Gid == NoGroup; }

    bool IsMemberOf(gid_t group) const;

    TString ToString() const {
        return User() + ":" + Group();
    }

    TError Load(const rpc::TCred &cred, bool strict = true);
    void Dump(rpc::TCred &cred);
};

void InitCapabilities();

struct TCapabilities {
    uint64_t Permitted = 0;

    TError Change(const TString &name, bool set);
    TError Parse(const TString &str);
    TString Format() const;
    TError Apply(int mask) const;
    TError ApplyLimit() const;
    TError ApplyAmbient() const;
    TError ApplyEffective() const;
    TError Get(pid_t pid, int type);
    void Dump();
    friend std::ostream& operator<<(std::ostream& os, const TCapabilities &c) {
        return os << c.Format();
    }
    bool HasSetUidGid() const;

    TError Load(const rpc::TCapabilities &cap);
    void Dump(rpc::TCapabilities &cap) const;
};

extern bool HasAmbientCapabilities;
extern TCapabilities NoCapabilities;
extern TCapabilities PortoInitCapabilities;
extern TCapabilities HelperCapabilities;
extern TCapabilities MemCgCapabilities;
extern TCapabilities PidNsCapabilities;
extern TCapabilities NetNsCapabilities;
extern TCapabilities HostCapAllowed;
extern TCapabilities ChrootCapBound;
extern TCapabilities HostCapBound;
extern TCapabilities AllCapabilities;
