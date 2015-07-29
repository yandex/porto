#pragma once

#include <string>
#include <array>

#include "common.hpp"
#include "util/unix.hpp"

class TNamespaceSnapshot : public TNonCopyable {
public:
    static const int nrNs = 5;
private:
    std::array<int, nrNs> nsFd;
    int RootFd, CwdFd;
    TError OpenProcPidFd(int pid, std::string name, int &fd);

public:
    TNamespaceSnapshot() : RootFd(-1), CwdFd(-1) { nsFd.fill(-1); }
    ~TNamespaceSnapshot() { Close(); }
    TError Open(int pid, std::set<std::string> ns = { "user", "ipc", "uts", "net", "pid", "mnt", });
    TError Chroot() const;
    TError Attach() const;
    void Close();
    bool Valid() const;
    bool HasNs(const std::string &ns) const;
};
