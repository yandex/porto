#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <iostream>
#include <memory>
#include <set>

#include <sys/mount.h>

#include "error.hpp"

class TMount {
    std::string device;
    std::string mountpoint;
    std::string vfstype;
    std::set<std::string> flags;

public:
    TMount(const std::string &device, const std::string &mountpoint, const std::string &vfstype,
           std::set<std::string> flags) :
        device (device), mountpoint (mountpoint), vfstype (vfstype),
        flags (flags) {}

    friend std::ostream& operator<<(std::ostream& os, const TMount& m) {
        os << m.device << " " << m.mountpoint << " ";
        for (auto f : m.flags)
            os << f << " ";

        return os;
    }

    friend bool operator==(const TMount& m1, const TMount& m2) {
        return m1.device == m2.device &&
            m1.mountpoint == m2.mountpoint &&
            m1.vfstype == m2.vfstype;
    }

    const std::string Mountpoint() {
        return mountpoint;
    }

    const std::string VFSType() {
        return vfstype;
    }

    const std::string ParentFolder() {
        return mountpoint.substr(0, mountpoint.find_last_of("/"));
    }

    std::set<std::string> const Flags() {
        return flags;
    }

    TError Mount(bool rdonly = false, bool bind = false);
    TError Umount();
};

class TMountSnapshot {
public:
    TError Mounts(std::set<std::shared_ptr<TMount>> &mounts);
};

#endif
