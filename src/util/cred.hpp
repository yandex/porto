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

void InitPortoGroups();

constexpr uid_t RootUser = (uid_t)0;
constexpr gid_t RootGroup = (gid_t)0;

constexpr uid_t NoUser = (uid_t)-1;
constexpr gid_t NoGroup = (gid_t)-1;

extern gid_t PortoGroup;

static constexpr const char * CRED_POSTFORK_TAINT_MESSAGE = "Credential function may deadlock in post-fork context";

class TCred {
    uid_t Uid;
    gid_t Gid;
    std::string UName;
    std::string GName;

    void UpdateUserName() {
        UName = RootUser == Uid ? "root" : UserName(Uid);
    }

    void UpdateGroupName() {
        GName = RootGroup == Gid ? "root" : GroupName(Gid);
    }

    static std::string GetMapping(uint32_t id);

public:
    std::vector<gid_t> Groups;

    TCred(uid_t uid, gid_t gid) : Uid(uid), Gid(gid) {
        UpdateUserName();
        UpdateGroupName();
    }

    TCred() : Uid(NoUser), Gid(NoGroup) {}

    static TCred Current();

    void Enter() const;
    void Leave() const;

    TError Init(const std::string &user);
    TError InitGroups(const std::string &user);

    TError Apply() const;
    TError SetupMapping(pid_t pid) const;

    uid_t GetUid() const {
        return Uid;
    }

    gid_t GetGid() const {
        return Gid;
    }

    void SetUid(uid_t uid) {
        Uid = uid;
        UpdateUserName();
    }

    void SetGid(gid_t gid) {
        Gid = gid;
        UpdateGroupName();
    }

    std::string User() const {
        return UName;
    }

    std::string Group() const {
        return GName;
    }

    bool IsRootUser() const { return Uid == RootUser; }
    bool IsRootGroup() const { return Gid == RootGroup; }

    bool IsUnknown() const { return Uid == NoUser && Gid == NoGroup; }

    bool IsMemberOf(gid_t group) const;

    std::string ToString() const {
        return User() + ":" + Group();
    }

    TError Load(const rpc::TCred &cred, bool strict = true);
    void Dump(rpc::TCred &cred);
};

void InitCapabilities();

struct TCapabilities {
    uint64_t Permitted = 0;

    TError Change(const std::string &name, bool set);
    TError Parse(const std::string &str);
    std::string Format() const;
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
extern TCapabilities SysAdminCapability;
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
extern TCapabilities SysBootCapability;
