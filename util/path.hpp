#ifndef __PATH_HPP__
#define __PATH_HPP__

#include <string>

#include "error.hpp"

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

public:
    TPath(const std::string &path) : Path(path) {}
    TPath(const char *path) : Path(path) {}
    TPath() : Path("") {}

    TPath operator+(const std::string &component) {
        return TPath(Path + component);
    }

    friend bool operator==(const TPath& a, const TPath& b) {
        return a.ToString() == b.ToString();
    }

    TPath DirName() const;
    std::string BaseName();

    EFileType GetType() const;
    unsigned int GetMode() const;
    std::string ToString() const;
    bool Exists() const;
    bool AccessOk(EFileAccess type) const;
    void AddComponent(const std::string &component);
    TError Chdir() const;
    TError Chroot() const;
    TError Chown(const std::string &user, const std::string &group) const;
    TError ReadLink(TPath &value) const;
};

#endif
