#pragma once

#include <string>
#include <functional>

#include "error.hpp"
#include "util/cred.hpp"

enum class EFileType {
    Regular,
    Directory,
    Block,
    Character,
    Fifo,
    Link,
    Socket,
    Unknown,
    Any
};

enum class EFileAccess {
    Read,
    Write,
    Execute
};

class TPath {
    std::string Path;

    std::string DirNameStr() const;
    TError RegularCopy(const TPath &to, unsigned int mode) const;
    unsigned int Stat(std::function<unsigned int(struct stat *st)> f) const;

public:
    TPath(const std::string &path) : Path(path) {}
    TPath(const char *path) : Path(path) {}
    TPath() : Path("") {}

    TPath operator+(const TPath &p) const {
        return TPath(Path + p.ToString());
    }

    friend bool operator==(const TPath& a, const TPath& b) {
        return a.ToString() == b.ToString();
    }

    friend bool operator<(const TPath& a, const TPath& b) {
        return a.ToString() < b.ToString();
    }

    friend std::ostream& operator<<(std::ostream& os, const TPath& path) {
        return os << path.ToString();
    }

    TPath DirName() const;
    std::string BaseName();

    EFileType GetType() const;
    unsigned int GetMode() const;
    unsigned int GetDev() const;
    unsigned int GetUid() const;
    unsigned int GetGid() const;
    std::string ToString() const;
    bool Exists() const;
    bool AccessOk(EFileAccess type) const;
    TPath AddComponent(const std::string &component) const;
    TError Chdir() const;
    TError Chroot() const;
    TError Chown(const TCred &cred) const;
    TError Chown(const std::string &user, const std::string &group) const;
    TError Chown(unsigned int uid, unsigned int gid) const;
    TError Chmod(const int mode) const;
    TError ReadLink(TPath &value) const;
    TError Copy(const TPath &to) const;
    TError Symlink(const TPath &to) const;
    TError Mkfifo(unsigned int mode) const;
    TError Mknod(unsigned int mode, unsigned int dev) const;
};
