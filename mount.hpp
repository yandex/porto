#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <set>
#include <iostream>
#include <memory>
#include <list>

#include <sys/mount.h>

#include "log.hpp"
#include "stringutil.hpp"

class TMount {
    string device;
    string mountpoint;
    string vfstype;
    set<string> flags;

    unsigned long mountflags = 0;

public:
    TMount(string mounts_line);

    TMount(string device, string mountpoint, string vfstype,
           unsigned long mountflags, set<string> flags) :
        device (device), mountpoint (mountpoint), vfstype (vfstype),
        flags (flags), mountflags (mountflags) {}

    friend ostream& operator<<(ostream& os, const TMount& m) {
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

    const string Mountpoint() {
        return mountpoint;
    }

    const string VFSType() {
        return vfstype;
    }

    const string ParentFolder() {
        return mountpoint.substr(0, mountpoint.find_last_of("/"));
    }

    set<string> const Flags() {
        return flags;
    }

    void Mount() {
        int ret = mount(device.c_str(), mountpoint.c_str(), vfstype.c_str(),
                        mountflags, CommaSeparatedList(flags).c_str());

        TLogger::LogAction("mount " + mountpoint, ret, errno);

        if (ret) {
            if (errno == EBUSY)
                throw "Already mounted";
            else
                throw "Cannot mount filesystem " + mountpoint;
        }
    }

    void Umount () {
        int ret = umount(mountpoint.c_str());

        TLogger::LogAction("umount " + mountpoint, ret, errno);

        if (ret)
            throw "Cannot umount filesystem " + mountpoint;
    }
};

class TMountState {
    std::set<std::shared_ptr<TMount> > mounts;

public:
    TMountState();

    std::set<std::shared_ptr<TMount> > const& Mounts();

    friend ostream& operator<<(std::ostream& os, const TMountState& ms);
};

#endif
