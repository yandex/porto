#pragma once

#include <string>
#include <array>

#include "common.hpp"
#include "util/unix.hpp"

bool InPidNamespace(pid_t pid1, pid_t pid2);

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
    TNamespaceFd Ipc;
    TNamespaceFd Uts;
    TNamespaceFd Net;
    TNamespaceFd Pid;
    TNamespaceFd Mnt;
    TNamespaceFd Root;
    TNamespaceFd Cwd;
    TNamespaceSnapshot() { }
    TError Open(int pid);
    TError Enter() const;
};
