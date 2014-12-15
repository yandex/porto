#ifndef __MOUNT_HPP__
#define __MOUNT_HPP__

#include <string>
#include <iostream>
#include <memory>
#include <set>

#include "error.hpp"
#include "util/path.hpp"

extern "C" {
#include <sys/mount.h>
}

class TMount {
    TPath Source;
    TPath Target;
    std::string Type;
    std::set<std::string> Data;

public:
    TMount(const TPath &source, const TPath &target, const std::string &type, std::set<std::string> data) :
        Source(source), Target(target), Type(type),
        Data(data) {}

    friend bool operator==(const TMount& m1, const TMount& m2) {
        return m1.Source == m2.Source &&
            m1.Target == m2.Target &&
            m1.Type == m2.Type;
    }

    const std::string GetMountpoint() const {
        return Target.ToString();
    }

    const std::string GetType() const {
        return Type;
    }

    std::set<std::string> const GetData() const {
        return Data;
    }

    TError Mount(unsigned long flags = 0) const;
    TError Bind(bool rdonly, unsigned long flags = 0) const;
    TError BindFile(bool rdonly, unsigned long flags = 0) const;
    TError BindDir(bool rdonly, unsigned long flags = 0) const;
    TError MountDir(unsigned long flags = 0) const;
    TError Umount() const;

    friend std::ostream& operator<<(std::ostream& stream, const TMount& mount) {
        stream << mount.Source.ToString() << " " << mount.Target.ToString() << " " << mount.Type << " ";

        for (auto &f : mount.Data)
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

class TLoopMount {
    TPath Source, Target;
    std::string Type;
    std::string LoopDev = "";
    TError FindDev();
public:
    TLoopMount(const TPath &source, const TPath &target, const std::string &type) : Source(source), Target(target), Type(type) {}
    TError Mount();
    TError Umount();
};

#endif
