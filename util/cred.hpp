#ifndef __CRED_H__
#define __CRED_H__

#include <string>

#include "common.hpp"

class TUserEntry : public TNonCopyable {
protected:
    std::string Name;
    int Id;
public:
    TUserEntry(const std::string &name) : Name(name), Id(-1) {}
    TUserEntry(const int id) : Name(""), Id(id) {}
    std::string GetName();
    int GetId();
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

    TCred(uid_t uid, gid_t gid) : Uid(uid), Gid(gid) {}
    TCred() : Uid(0), Gid(0) {}

    bool IsRoot() const {
        return Uid == 0 || Gid == 0;
    }

    bool operator== (const TCred &cred) const {
        return (cred.Uid == Uid || cred.Gid == Gid);
    }

    bool operator!= (const TCred &cred) const {
        return (cred.Uid != Uid && cred.Gid != Gid);
    }
};

#endif /* __CRED_H__ */
