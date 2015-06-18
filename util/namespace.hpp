#pragma once

#include <map>
#include <string>

#include "common.hpp"
#include "util/unix.hpp"

class TNamespaceSnapshot : public TNonCopyable {
    std::map<int,int> nsToFd;
    TScopedFd Root, Cwd;
    TError OpenFd(int pid, std::string v, TScopedFd &fd);

public:
    TNamespaceSnapshot() {}
    ~TNamespaceSnapshot() { Destroy(); }
    TError Create(int pid, bool only_mnt = false);
    TError Chroot() const;
    TError Attach() const;
    void Destroy();
    bool Valid() const;
};
