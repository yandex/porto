#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <iostream>
#include <memory>
#include <set>

#include "error.hpp"

extern "C" {
#include <sys/mount.h>
}

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

    TError Mount(unsigned long flags = 0) const;
    TError Remount() const { return Mount(MS_REMOUNT); }
    TError Bind() const { return Mount(MS_BIND); }
    TError MountPrivate() { return Mount(MS_PRIVATE); }
    TError Umount() const;

    friend std::ostream& operator<<(std::ostream& stream, const TMount& mount) {
        return stream << mount.Device << " " << mount.Mountpoint << " " << mount.Vfstype;
    }
};

class TMountSnapshot {
    std::string Path;
public:
    TMountSnapshot(const std::string &path = "/proc/self/mounts") : Path(path) {}
    TError Mounts(std::set<std::shared_ptr<TMount>> &mounts) const;
    TError RemountSlave();
};

#endif
