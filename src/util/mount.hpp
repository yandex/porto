#pragma once

#include <string>
#include <iostream>
#include <memory>
#include <vector>
#include <algorithm>

#include "error.hpp"
#include "util/path.hpp"

extern "C" {
#include <sys/mount.h>
}

class TMount {
    TPath Source;
    TPath Target;
    std::string Type;
    std::vector<std::string> Data;

public:
    TMount() {}
    TMount(const TPath &source,
           const TPath &target,
           const std::string &type,
           std::vector<std::string> data) :
        Source(source), Target(target), Type(type), Data(data) {}

    friend bool operator==(const TMount& m1, const TMount& m2) {
        return m1.Source == m2.Source &&
            m1.Target == m2.Target &&
            m1.Type == m2.Type;
    }

    const TPath GetSource() const { return Source; }

    const TPath GetMountpoint() const {
        return Target;
    }

    const std::string GetType() const {
        return Type;
    }

    const std::vector<std::string> GetData() const {
        return Data;
    }

    TError Find(TPath path, const TPath mounts = "/proc/self/mounts");

    static TError Snapshot(std::vector<std::shared_ptr<TMount>> &result,
                           const TPath mounts = "/proc/self/mounts");

    TError Mount(unsigned long flags = 0) const;
    TError Bind(bool rdonly, unsigned long flags = 0) const;
    TError BindFile(bool rdonly, unsigned long flags = 0) const;
    TError BindDir(bool rdonly, unsigned long flags = 0) const;
    TError MountDir(unsigned long flags = 0) const;
    TError Umount(int flags = UMOUNT_NOFOLLOW) const;
    TError Move(TPath destination);

    bool HasFlag(const std::string flag) const {
        return std::find(Data.begin(), Data.end(), flag) != Data.end();
    }

    friend std::ostream& operator<<(std::ostream& stream, const TMount& mount) {
        stream << mount.Source << " " << mount.Target << " " << mount.Type << " ";

        for (auto &f : mount.Data) {
            stream << f;
            if (&f != &mount.Data.back())
                stream << ",";
        }

        return stream;
    }
};

TError SetupLoopDevice(TPath image, int &dev);
TError PutLoopDev(const int nr);
