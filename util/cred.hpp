#ifndef __CRED_H__
#define __CRED_H__

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
