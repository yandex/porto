#pragma once

#include <string>
#include <vector>
#include <list>

#include "util/error.hpp"
#include "util/cred.hpp"
#include "util/string.hpp"

extern "C" {
#include <sys/mount.h>
}

struct TStatFS {
    uint64_t SpaceUsage;
    uint64_t SpaceAvail;
    uint64_t InodeUsage;
    uint64_t InodeAvail;
    bool ReadOnly;
    bool Secure; /* nodev and noexec or nosuid  */

    void Init(const struct statfs &st);
    void Reset();
};

struct TMount;

class TPath {
    friend class TFile;
    std::string Path;

    std::string DirNameStr() const;
    TPath AddComponent(const TPath &component) const;

public:
    TPath(const std::string &path) : Path(path) {}
    TPath(const char *path) : Path(path) {}
    TPath() : Path("") {}

    bool IsAbsolute() const { return Path[0] == '/'; }

    bool IsSimple() const { return Path.find('/') == std::string::npos; }

    bool IsRoot() const { return Path == "/"; }

    bool IsEmpty() const { return Path.empty(); }

    bool IsNormal() const { return Path == NormalPath().Path; }

    bool IsInside(const TPath &base) const;

    bool IsDotDot() const {
        return Path[0] == '.' && Path[1] == '.' &&
              (Path[2] == '/' || Path[2] == '\0');
    }

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

    bool IsSameInode(const TPath &other) const;

    unsigned int GetDev() const;
    unsigned int GetBlockDev() const;

    int64_t SinceModificationMs() const;
    std::string ToString() const;
    bool Exists() const;

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
    TError Truncate(off_t size) const;
    TError RotateLog(off_t max_disk_usage, off_t &loss) const;
    TError Chattr(unsigned add_flags, unsigned del_flags) const;
    TError Touch() const;

    static const TFlagsNames MountFlags;
    static const TFlagsNames UmountFlags;
    static std::string MountFlagsToString(unsigned long flags);
    static std::string UmountFlagsToString(unsigned long flags);

    TError FindMount(TMount &mount) const;
    static TError ListAllMounts(std::list<TMount> &list);

    TError Mount(const TPath &source, const std::string &type, unsigned long flags,
                 const std::vector<std::string> &options) const;
    TError Bind(const TPath &source) const;
    TError BindAll(const TPath &source) const;
    TError Remount(unsigned long flags) const;
    TError BindRemount(const TPath &source, unsigned long flags) const;
    TError Umount(unsigned long flags) const;
    TError UmountAll() const;
    TError UmountNested() const;

    TError ReadAll(std::string &text, size_t max = 1048576) const;
    TError ReadLines(std::vector<std::string> &lines, size_t max = 1048576) const;
    TError ReadInt(int &value) const;

    TError WriteAll(const std::string &text) const;
    TError WritePrivate(const std::string &text) const;
};

struct TMount {
    TPath Source;
    TPath Target;
    std::string Type;
    std::string Options;

    bool HasOption(const std::string &option) const;

    friend std::ostream& operator<<(std::ostream& stream, const TMount& mount) {
        stream << mount.Source << " " << mount.Target
               << " -t " << mount.Type << " -o " << mount.Options;
        return stream;
    }
};

class TFile {
public:
    union {
        const int Fd;
        int SetFd;
    };
    TFile() : Fd(-1) { }
    ~TFile() { Close(); }
    TError Open(const TPath &path, int flags);
    TError OpenRead(const TPath &path);
    TError OpenWrite(const TPath &path);
    TError OpenReadWrite(const TPath &path);
    TError OpenTrunc(const TPath &path);
    TError OpenAppend(const TPath &path);
    TError OpenDir(const TPath &path);
    TError OpenDirStrict(const TPath &path);
    TError OpenPath(const TPath &path);
    TError CreateTemp(const TPath &path);
    TError Create(const TPath &path, int flags, int mode);
    TError CreateNew(const TPath &path, int mode);
    TError CreateTrunc(const TPath &path, int mode);
    void Close(void);
    static void CloseAll(std::vector<int> except);
    TPath RealPath(void) const;
    TPath ProcPath(void) const;
    TError ReadAll(std::string &text, size_t max) const;
    TError WriteAll(const std::string &text) const;
    static TError Chattr(int fd, unsigned add_flags, unsigned del_flags);
    int GetMountId(void) const;
    TError Dup(const TFile &other);
    TError OpenAt(const TFile &dir, const TPath &path, int flags, int mode);
    TError MkdirAt(const TPath &path, int mode) const;
    TError UnlinkAt(const TPath &path) const;
    TError RmdirAt(const TPath &path) const;
    TError RenameAt(const TPath &oldpath, const TPath &newpath) const;
    TError Chown(uid_t uid, gid_t gid) const;
    TError Chown(const TCred &cred) const {
        return Chown(cred.Uid, cred.Gid);
    }
    TError Chmod(mode_t mode) const;
    TError ChownAt(const TPath &path, uid_t uid, gid_t gid) const;
    TError ChownAt(const TPath &path, const TCred &cred) const {
        return ChownAt(path, cred.Uid, cred.Gid);
    }
    TError ChmodAt(const TPath &path, mode_t mode) const;
    TError Touch() const;
    TError WalkFollow(const TFile &dir, const TPath &path);
    TError WalkStrict(const TFile &dir, const TPath &path);
    TError ClearDirectory() const;
    TError Stat(struct stat &st) const;
    TError StatAt(const TPath &path, bool follow, struct stat &st) const;
    TError StatFS(TStatFS &result) const;

    enum AccessMode {
        E   = 000, /* Exists */
        X   = 001,
        W   = 002,
        R   = 004,
    };
    static bool Access(const struct stat &st, const TCred &cred, enum AccessMode mode);
    TError ReadAccess(const TCred &cred) const;
    TError WriteAccess(const TCred &cred) const;
};
