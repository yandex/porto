#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <set>
#include <iostream>

#include <sys/mount.h>

#include "log.hpp"
#include "stringutil.hpp"

using namespace std;

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
    set<TMount*> mounts;

public:
    void UpdateFromProcfs();
    
    ~TMountState() {
        for (auto m : mounts)
            delete m;
    }

    set<TMount*> const& Mounts() {
        return mounts;
    }

    friend ostream& operator<<(ostream& os, const TMountState& ms) {
        for (auto m : ms.mounts)
            os << *m << endl;

        return os;
    }
};
#endif
