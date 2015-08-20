#pragma once

#include <string>
#include <array>

#include "common.hpp"
#include "util/unix.hpp"

class TNamespaceFd : public TNonCopyable {
    int Fd;
public:
    TNamespaceFd() : Fd(-1) {}
    ~TNamespaceFd() { Close(); }
    bool IsOpened() const { return Fd >= 0; }
    TError Open(TPath path);
    TError Open(pid_t pid, std::string type);
    void Close();
    TError SetNs(int type = 0) const;
    TError Chroot() const;
    TError Chdir() const;
    bool operator==(const TNamespaceFd &other) const;
    bool operator!=(const TNamespaceFd &other) const;
};

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
