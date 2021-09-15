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
    bool Optional = false;

    static TError CheckPath(const TPath &path);

    TDevice() {}
    TDevice(const TPath &path, dev_t node) : Path(path), PathInside(path), Node(node) {}

    TError Parse(TTuple &opt, const TCred &cred);
    std::string FormatAccess() const;
    std::string Format() const;
    std::string CgroupRule(bool allow) const;
    TError Makedev(const TPath &root = "/") const;

    TError Load(const rpc::TContainerDevice &dev, const TCred &cred);
    void Dump(rpc::TContainerDevice &dev) const;
};

struct TDevices {
    std::vector<TDevice> Devices;
    bool NeedCgroup = false;
    bool AllOptional = false;

    TError Parse(const std::string &str, const TCred &cred);
    std::string Format() const;

    void PrepareForUserNs(const TCred &userNsCred);
    TError Makedev(const TPath &root = "/") const;
    TError Apply(const TCgroup &cg, bool rootUser, bool reset = false) const;

    TError InitDefault();
    void Merge(const TDevices &devices, bool overwrite = false, bool replace = false);
};
