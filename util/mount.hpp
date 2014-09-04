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

    const std::string GetMountpoint() const {
        return Mountpoint;
    }

    const std::string VFSType() const {
        return Vfstype;
    }

    const std::string ParentFolder() const {
        return Mountpoint.substr(0, Mountpoint.find_last_of("/"));
    }

    std::set<std::string> const GetFlags() const {
        return Flags;
    }

    TError Mount(bool rdonly = false, bool bind = false, bool remount = false) const;
    TError Remount() const { return Mount(false, false, true); }
    TError Bind() const { return Mount(false, true); }
    TError Umount() const;
};

class TMountSnapshot {
    std::string Path;
public:
    TMountSnapshot(const std::string &path = "/proc/self/mounts") : Path(path) {}
    TError Mounts(std::set<std::shared_ptr<TMount>> &mounts) const;
};

#endif
