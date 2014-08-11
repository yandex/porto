#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <iostream>
#include <memory>
#include <set>

#include <sys/mount.h>

#include "log.hpp"
#include "stringutil.hpp"

class TMount {
    std::string device;
    std::string mountpoint;
    std::string vfstype;
    std::set<std::string> flags;

    unsigned long mountflags = 0;

public:
    TMount(const std::string &mounts_line);

    TMount(const std::string &device, const std::string &mountpoint, const std::string &vfstype,
           unsigned long mountflags, std::set<std::string> flags) :
        device (device), mountpoint (mountpoint), vfstype (vfstype),
        flags (flags), mountflags (mountflags) {}

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

    TError Mount() {
        int ret = mount(device.c_str(), mountpoint.c_str(), vfstype.c_str(),
                        mountflags, CommaSeparatedList(flags).c_str());

        TLogger::LogAction("mount " + mountpoint, ret, errno);

        if (ret)
            return TError("Cannot mount filesystem " + mountpoint);

        return TError();
    }

    TError Umount () {
        int ret = umount(mountpoint.c_str());

        TLogger::LogAction("umount " + mountpoint, ret, errno);

        if (ret)
            return TError("Cannot umount filesystem " + mountpoint);

        return TError();
    }
};

class TMountSnapshot {
    std::set<std::shared_ptr<TMount> > mounts;

public:
    TMountSnapshot();

    std::set<std::shared_ptr<TMount> > const& Mounts();

    friend std::ostream& operator<<(std::ostream& os, const TMountSnapshot& ms);
};

#endif
