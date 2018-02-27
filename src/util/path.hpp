#pragma once

#include <string>
#include <vector>
#include <list>

#include "util/error.hpp"
#include "util/cred.hpp"
#include "util/string.hpp"

extern "C" {
#include <sys/mount.h>
#include <sys/stat.h>
#include <fts.h>
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

#ifndef MS_LAZYTIME
# define MS_LAZYTIME    (1<<25)
#endif

/* inverted flags with lower priority */
#define MS_ALLOW_WRITE  (1ull << 60)
#define MS_ALLOW_EXEC   (1ull << 61)
#define MS_ALLOW_SUID   (1ull << 62)
#define MS_ALLOW_DEV    (1ull << 63)

class TPath {
    friend class TFile;
    std::string Path;

    TPath AddComponent(const TPath &component) const;

public:
    TPath(const std::string &path) : Path(path) {}
    TPath(const char *path) : Path(path) {}
    TPath() : Path("") {}

    bool IsAbsolute() const { return Path[0] == '/'; }

    bool IsSimple() const { return Path.find('/') == std::string::npos; }

    bool IsRoot() const { return Path == "/"; }

    bool IsEmpty() const { return Path.empty(); }

    explicit operator bool() const { return !Path.empty(); }

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

    TPath NormalPath() const;

    TPath DirNameNormal() const;
    std::string BaseNameNormal() const;

    TPath DirName() const;
    std::string BaseName() const;

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

    TError Chown(uid_t uid, gid_t gid) const;

    TError Chown(const TCred &cred) const {
        return Chown(cred.Uid, cred.Gid);
    }

    TError Chmod(const int mode) const;
    TError ReadLink(TPath &value) const;
    TError Hardlink(const TPath &target) const;
    TError Symlink(const TPath &target) const;
    TError Mknod(unsigned int mode, unsigned int dev) const;
    TError Mkfile(unsigned int mode) const;
    TError Mkdir(unsigned int mode) const;
    TError MkdirAll(unsigned int mode) const;
    TError MkdirTmp(const TPath &parent, const std::string &prefix, unsigned int mode);
    TError Rmdir() const;
    TError Unlink() const;
    TError RemoveAll() const;
    TError Rename(const TPath &dest) const;
    TError ReadDirectory(std::vector<std::string> &result) const;
    TError ListSubdirs(std::vector<std::string> &result) const;
    TError ClearDirectory() const;
    TError StatFS(TStatFS &result) const;
    TError GetXAttr(const std::string &name, std::string &value) const;
    TError SetXAttr(const std::string &name, const std::string &value) const;
    TError Truncate(off_t size) const;
    TError RotateLog(off_t max_disk_usage, off_t &loss) const;
    TError Chattr(unsigned add_flags, unsigned del_flags) const;
    TError Touch() const;

    static std::string UmountFlagsToString(uint64_t flags);

    TError FindMount(TMount &mount) const;
    static TError ListAllMounts(std::list<TMount> &list);

    TError Mount(const TPath &source, const std::string &type, uint64_t flags,
                 const std::vector<std::string> &options) const;
    TError Bind(const TPath &source, uint64_t flags = 0) const;
    TError MoveMount(const TPath &target) const;
    TError Remount(uint64_t flags) const;
    TError BindRemount(const TPath &source, uint64_t flags) const;
    TError Umount(uint64_t flags) const;
    TError UmountAll() const;
    TError UmountNested() const;

    TError ReadAll(std::string &text, size_t max = 1048576) const;
    TError ReadLines(std::vector<std::string> &lines, size_t max = 1048576) const;
    TError ReadInt(int &value) const;

    TError WriteAll(const std::string &text) const;
    TError WriteAtomic(const std::string &text) const;
    TError WritePrivate(const std::string &text) const;
};

// FIXME replace with streaming someday
constexpr const size_t MOUNT_INFO_LIMIT = 64 << 20;

struct TMount {
    TPath Source;
    TPath Target;
    std::string Type;
    std::string Options;

    int MountId;
    int ParentId;
    dev_t Device;
    TPath BindPath; /* path within filesystem */
    uint64_t MntFlags;
    std::vector<std::string> OptFields;

    static std::string Demangle(const std::string &s);
    TError ParseMountinfo(const std::string &line);
    bool HasOption(const std::string &option) const;

    friend std::ostream& operator<<(std::ostream& stream, const TMount& mount) {
        stream << mount.Source << " " << mount.Target
               << " -t " << mount.Type << " -o " << mount.Options;
        return stream;
    }

    static TError ParseFlags(const std::string &str, uint64_t &flags, uint64_t allowed = -1);
    static std::string FormatFlags(uint64_t flags);
};

class TFile {
public:
    union {
        const int Fd;
        int SetFd;
    };
    TFile() : Fd(-1) { }
    TFile(int fd) : Fd(fd) { }
    ~TFile() { Close(); }
    explicit operator bool() const { return Fd >= 0; }
    TError Open(const TPath &path, int flags);
    TError OpenRead(const TPath &path);
    TError OpenWrite(const TPath &path);
    TError OpenReadWrite(const TPath &path);
    TError OpenTrunc(const TPath &path);
    TError OpenAppend(const TPath &path);
    TError OpenDir(const TPath &path);
    TError OpenDirStrict(const TPath &path);
    TError OpenPath(const TPath &path);
    TError CreateTemporary(TPath &temp, int flags = 0);
    TError CreateUnnamed(const TPath &dir, int flags = 0);
    TError Create(const TPath &path, int flags, int mode);
    TError CreateNew(const TPath &path, int mode);
    TError CreateTrunc(const TPath &path, int mode);
    TError CreatePath(const TPath &path, const TCred &cred, const TPath &bound = "");
    void Close(void);
    static void CloseAll(std::vector<int> except);
    TPath RealPath(void) const;
    TPath ProcPath(void) const;
    TError Read(std::string &text) const;
    TError ReadAll(std::string &text, size_t max) const;
    TError ReadEnds(std::string &text, size_t max) const;
    TError Truncate(off_t size) const;
    TError WriteAll(const std::string &text) const;
    static TError Chattr(int fd, unsigned add_flags, unsigned del_flags);
    int GetMountId(const TPath relative = TPath("")) const;
    TError Dup(const TFile &other);
    TError OpenAt(const TFile &dir, const TPath &path, int flags, int mode);
    TError MkdirAt(const TPath &path, int mode) const;
    TError UnlinkAt(const TPath &path) const;
    TError RmdirAt(const TPath &path) const;
    TError RemoveAt(const TPath &path) const;
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
    TError GetXAttr(const std::string &name, std::string &value) const;
    TError SetXAttr(const std::string &name, const std::string &value) const;
    TError WalkFollow(const TFile &dir, const TPath &path);
    TError WalkStrict(const TFile &dir, const TPath &path);
    TError Chdir() const;
    TError ClearDirectory() const;
    TError Stat(struct stat &st) const;
    TError StatAt(const TPath &path, bool follow, struct stat &st) const;
    bool ExistsAt(const TPath &path) const;

    TError StatFS(TStatFS &result) const;
    TError PivotRoot() const;

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

class TPathWalk {
public:
    FTS *Fts = nullptr;
    FTSENT *Ent = nullptr;
    TPath Path;
    struct stat *Stat;
    bool Postorder = false;

    static int CompareNames(const FTSENT **a, const FTSENT **b);
    static int CompareInodes(const FTSENT **a, const FTSENT **b);

    TPathWalk() {}
    ~TPathWalk() { Close(); }
    TError Open(const TPath &patht, int fts_flags = FTS_COMFOLLOW | FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, int (*compar)(const FTSENT **, const FTSENT **) = nullptr);
    TError OpenScan(const TPath &path);
    TError OpenList(const TPath &path);
    TError Next();
    std::string Name() { return Ent ? Ent->fts_name : ""; }
    int Level() { return Ent ? Ent->fts_level : -2; }
    void Close();
};
