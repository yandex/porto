#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <iostream>
#include <memory>
#include <set>

#include "error.hpp"

class TMount {
    std::string Device;
    std::string Mountpoint;
    std::string Vfstype;
    std::set<std::string> Flags;

public:
    TMount(const std::string &device, const std::string &mountpoint, const std::string &vfstype,
           std::set<std::string> flags) :
        Device(device), Mountpoint(mountpoint), Vfstype(vfstype),
        Flags(flags) {}

    friend bool operator==(const TMount& m1, const TMount& m2) {
        return m1.Device == m2.Device &&
            m1.Mountpoint == m2.Mountpoint &&
            m1.Vfstype == m2.Vfstype;
    }

    const std::string GetMountpoint() {
        return Mountpoint;
    }

    const std::string VFSType() {
        return Vfstype;
    }

    const std::string ParentFolder() {
        return Mountpoint.substr(0, Mountpoint.find_last_of("/"));
    }

    std::set<std::string> const GetFlags() {
        return Flags;
    }

    TError Mount(bool rdonly = false, bool bind = false, bool remount = false);
    TError Remount() { return Mount(false, false, true); }
    TError Bind() { return Mount(false, true); }
    TError Umount();
};

class TMountSnapshot {
public:
    TError Mounts(std::set<std::shared_ptr<TMount>> &mounts);
};

#endif
