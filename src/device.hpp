#pragma once

#include <string>
#include "util/path.hpp"
#include "util/cred.hpp"

struct TDevice {
    TPath Path;
    std::string Name;
    dev_t Device;
    mode_t Mode;
    uid_t User;
    gid_t Group;
    bool Read;
    bool Write;
    bool Mknod;
    bool Wildcard;
    bool Privileged;

    TError Parse(const std::string &cfg);
    std::string Format() const;
    std::string CgroupRule(bool allow) const;
    TError Permitted(const TCred &cred) const;
    TError Makedev(const TPath &root) const;
};
