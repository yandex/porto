#pragma once

#include <string>
#include "util/string.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"

class TCgroup;

struct TDevice {
    TPath Path;
    TPath PathInside;

    dev_t Node = 0;
    uid_t Uid = 0;
    gid_t Gid = 0;
    mode_t Mode = S_IFCHR | 0666;

    bool MayRead = true;
    bool MayWrite = true;
    bool MayMknod = true;

    bool Wildcard = false;
    bool Privileged = false;

    static TError CheckPath(const TPath &path);
    TError Init(const TPath &path);

    TDevice() {}
    TDevice(const TPath &path, dev_t node) : Path(path), PathInside(path), Node(node) {}

    TError Parse(TTuple &opt);
    std::string Format() const;
    std::string CgroupRule(bool allow) const;
    TError Permitted(const TCred &cred) const;
    TError Makedev(const TPath &root = "/") const;
};

struct TDevices {
    std::vector<TDevice> Devices;
    bool NeedCgroup = false;

    TError Parse(const std::string &str);
    std::string Format() const;

    TError Permitted(const TCred &cred) const;
    TError Makedev(const TPath &root = "/") const;
    TError Apply(const TCgroup &cg, bool reset = false) const;

    TError InitDefault();
    void Merge(const TDevices &devices, bool overwrite = false, bool replace = false);
};
