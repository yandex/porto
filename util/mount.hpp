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
    std::string Source;
    std::string Target;
    std::string Vfstype;
    std::set<std::string> Flags;

public:
    TMount(const std::string &source, const std::string &target, const std::string &vfstype,
           std::set<std::string> flags) :
        Source(source), Target(target), Vfstype(vfstype),
        Flags(flags) {}

    friend bool operator==(const TMount& m1, const TMount& m2) {
        return m1.Source == m2.Source &&
            m1.Target == m2.Target &&
            m1.Vfstype == m2.Vfstype;
    }

    const std::string GetMountpoint() const {
        return Target;
    }

    const std::string VFSType() const {
        return Vfstype;
    }

    const std::string ParentFolder() const {
        return Target.substr(0, Target.find_last_of("/"));
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
        stream << mount.Source << " " << mount.Target << " " << mount.Vfstype << " ";

        for (auto &f : mount.Flags)
            stream << f << ",";

        return stream;
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
