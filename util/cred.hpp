#pragma once

#include <string>
#include <set>

#include "common.hpp"
#include "util/string.hpp"

class TPath;

class TUserEntry : public TNonCopyable {
protected:
    std::string Name;
    int Id;
public:
    TUserEntry(const std::string &name) :
        Name(!name.empty() && StringOnlyDigits(name) ? "" : name),
          Id(!name.empty() && StringOnlyDigits(name) ? stoi(name) : -1) {}
    TUserEntry(const int id) : Name(""), Id(id) {}
    std::string GetName() const;
    int GetId() const;
    TError LoadFromFile(const TPath &path);
};

class TUser : public TUserEntry {
public:
    TUser(const std::string &name) : TUserEntry(name) {}
    TUser(const int id) : TUserEntry(id) {}
    TError Load();
};

class TGroup : public TUserEntry {
public:
    TGroup(const std::string &name) : TUserEntry(name) {}
    TGroup(const int id) : TUserEntry(id) {}
    TError Load();
};

struct TCred {
public:
    uid_t Uid;
    gid_t Gid;
    std::vector<gid_t> Groups;

    TCred(uid_t uid, gid_t gid) : Uid(uid), Gid(gid) {}
    TCred() : Uid(0), Gid(0) {}

    bool operator== (const TCred &cred) const {
        return (cred.Uid == Uid || cred.Gid == Gid);
    }

    bool operator!= (const TCred &cred) const {
        return (cred.Uid != Uid && cred.Gid != Gid);
    }

    bool MemberOf(gid_t gid) const;

    std::string UserAsString() const;
    std::string GroupAsString() const;

    bool IsRoot() const {
        return Uid == 0 || Gid == 0;
    }

    bool IsPrivileged() const;

    TError Parse(const std::string &user, const std::string &group);
};

class TCredConf : public TNonCopyable {
private:
    std::set<int> PrivilegedUid, PrivilegedGid;
    std::set<int> RestrictedRootUid, RestrictedRootGid;
    gid_t PortoGid;
public:
    void Load();
    bool PrivilegedUser(const TCred &cred);
    bool RestrictedUser(const TCred &cred);
    gid_t GetPortoGid() { return PortoGid; }
};

extern TCredConf CredConf;
