#pragma once

#include <string>
#include <array>

#include "common.hpp"
#include "util/unix.hpp"
#include "util/path.hpp"

class TNamespaceFd : public TNonCopyable {
    int Fd;
public:
    TNamespaceFd() : Fd(-1) {}
    ~TNamespaceFd() { Close(); }
    TNamespaceFd& operator=(TNamespaceFd&& other) {
        Close();
        Fd = other.Fd;
        other.Fd = -1;
        return *this;
    }
    TError Open(TPath path);
    TError Open(pid_t pid, std::string type);
    int GetFd() const { return Fd; }
    void Close();
    TError SetNs(int type = 0) const;
    TError Chroot() const;
    TError Chdir() const;
    ino_t Inode() const;
    static ino_t PidInode(pid_t pid, std::string type);
};
