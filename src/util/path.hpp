#pragma once

#include <string>
#include <functional>

#include "util/error.hpp"
#include "string.hpp"
#include "util/cred.hpp"

struct TStatFS {
    uint64_t SpaceUsage;
    uint64_t SpaceAvail;
    uint64_t InodeUsage;
    uint64_t InodeAvail;

    void Reset() {
        SpaceUsage = SpaceAvail = InodeUsage = InodeAvail = 0;
    }
};

class TPath {
    std::string Path;

    std::string DirNameStr() const;
    TPath AddComponent(const TPath &component) const;

public:
    TPath(const std::string &path) : Path(path) {}
    TPath(const char *path) : Path(path) {}
    TPath() : Path("") {}

    bool IsAbsolute() const { return Path[0] == '/'; }

    bool IsRoot() const { return Path == "/"; }

    bool IsEmpty() const { return Path.empty(); }

    bool IsNormal() const { return Path == NormalPath().Path; }

    const char *c_str() const noexcept { return Path.c_str(); }

    TPath operator+(const TPath &p) const {
        return TPath(Path + p.ToString());
    }

    friend bool operator==(const TPath& a, const TPath& b) {
        return a.ToString() == b.ToString();
    }

    friend bool operator!=(const TPath& a, const TPath& b) {
        return a.ToString() != b.ToString();
    }

    friend bool operator<(const TPath& a, const TPath& b) {
        return a.ToString() < b.ToString();
    }

    friend bool operator>(const TPath& a, const TPath& b) {
        return a.ToString() > b.ToString();
    }

    friend std::ostream& operator<<(std::ostream& os, const TPath& path) {
        return os << path.ToString();
    }

    friend TPath operator/(const TPath& a, const TPath &b) {
        return a.AddComponent(b);
    }

    TPath& operator/=(const TPath &b) {
        *this = AddComponent(b);
        return *this;
    }

    TPath DirName() const;
    std::string BaseName() const;
    TPath NormalPath() const;
    TPath AbsolutePath() const;
    TPath RealPath() const;
    TPath InnerPath(const TPath &path, bool absolute = true) const;

    TError StatStrict(struct stat &st) const;
    TError StatFollow(struct stat &st) const;

    bool IsRegularStrict() const;
    bool IsRegularFollow() const;

    bool IsDirectoryStrict() const;
    bool IsDirectoryFollow() const;

    unsigned int GetDev() const;
    unsigned int GetBlockDev() const;

    int64_t SinceModificationMs() const;
    std::string ToString() const;
    bool Exists() const;

    enum Access {
        X   = 001,
        W   = 002,
        R   = 004,
        RW  = 006,
        RWX = 007,
        U   = 010,  /* owner user */
        WU  = 012,
        RU  = 014,
        RWU = 016, /* (read and write) or owner user */
    };

    bool HasAccess(const TCred &cred, enum Access mask) const;
    bool CanRead(const TCred &cred) const { return HasAccess(cred, W); }
    bool CanWrite(const TCred &cred) const { return HasAccess(cred, R); }

    TError Chdir() const;
    TError Chroot() const;
    TError PivotRoot() const;

    TError Chown(uid_t uid, gid_t gid) const;

    TError Chown(const TCred &cred) const {
        return Chown(cred.Uid, cred.Gid);
    }

    TError Chmod(const int mode) const;
    TError ReadLink(TPath &value) const;
    TError Symlink(const TPath &target) const;
    TError Mknod(unsigned int mode, unsigned int dev) const;
    TError Mkfile(unsigned int mode) const;
    TError Mkdir(unsigned int mode) const;
    TError MkdirAll(unsigned int mode) const;
    TError MkdirTmp(const TPath &parent, const std::string &prefix, unsigned int mode);
    TError CreateAll(unsigned int mode) const;
    TError Rmdir() const;
    TError Unlink() const;
    TError RemoveAll() const;
    TError Rename(const TPath &dest) const;
    TError ReadDirectory(std::vector<std::string> &result) const;
    TError ListSubdirs(std::vector<std::string> &result) const;
    TError ClearDirectory() const;
    TError StatFS(TStatFS &result) const;
    TError SetXAttr(const std::string name, const std::string value) const;
    TError RotateLog(off_t max_disk_usage, off_t &loss) const;
    TError Chattr(unsigned add_flags, unsigned del_flags) const;


    static const TFlagsNames MountFlags;
    static const TFlagsNames UmountFlags;
    static std::string MountFlagsToString(unsigned long flags);
    static std::string UmountFlagsToString(unsigned long flags);

    TError Mount(TPath source, std::string type, unsigned long flags,
                 std::vector<std::string> options) const;
    TError Bind(TPath source) const;
    TError Remount(unsigned long flags) const;
    TError BindRemount(TPath source, unsigned long flags) const;
    TError Umount(unsigned long flags) const;
    TError UmountAll() const;

    TError ReadAll(std::string &text, size_t max = 1048576) const;
    TError ReadLines(std::vector<std::string> &lines, size_t max = 1048576) const;
    TError ReadInt(int &value) const;

    TError WriteAll(const std::string &text) const;
};
