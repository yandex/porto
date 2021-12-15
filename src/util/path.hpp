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
    uint64_t MntFlags;
    uint32_t FsType;

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
private:
    std::string Path;
    friend class TFile;

    TPath AddComponent(const TPath &component) const;

    static void RootFilesChownFilter(const struct stat& st, uid_t &uid, gid_t &gid) {
        if (st.st_uid != 0)
            uid = (uid_t)-1;
        if (st.st_gid != 0)
            gid = (gid_t)-1;
    }

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

    bool StartsWithDotDot() const {
        return Path[0] == '.' && Path[1] == '.' &&
              (Path[2] == '/' || Path[2] == '\0');
    }

    std::vector<std::string> Components() const;

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

    TPath AbsolutePath(const TPath &base = "") const;
    TPath RelativePath(const TPath &base) const;

    TPath RealPath() const;
    TPath InnerPath(const TPath &path, bool absolute = true) const;

    TError StatStrict(struct stat &st) const;
    TError StatFollow(struct stat &st) const;

    bool IsRegularStrict() const;
    bool IsRegularFollow() const;

    bool IsDirectoryStrict() const;
    bool IsDirectoryFollow() const;

    bool IsSocket() const;

    bool IsSameInode(const TPath &other) const;

    dev_t GetDev() const;
    dev_t GetBlockDev() const;
    static TError GetDevName(dev_t dev, std::string &name);

    int64_t SinceModificationMs() const;
    std::string ToString() const;
    bool Exists() const;
    bool PathExists() const; /* or dangling symlink */

    TError Chdir() const;
    TError Chroot() const;

    TError Chown(uid_t uid, gid_t gid) const;
    TError Chown(const TCred &cred) const {
        return Chown(cred.GetUid(), cred.GetGid());
    }

    TError Lchown(uid_t uid, gid_t gid) const;
    TError Lchown(const TCred &cred) const {
        return Lchown(cred.GetUid(), cred.GetGid());
    }

    typedef void (*TChownFilter)(const struct stat&, uid_t &uid, gid_t &gid);
    TError ChownRecursive(uid_t uid, gid_t gid, TChownFilter filter = nullptr) const;
    TError ChownRecursive(const TCred &cred, TChownFilter filter = nullptr) const {
        return ChownRecursive(cred.GetUid(), cred.GetGid(), filter);
    }

    TError ChownRecursiveRootFiles(uid_t uid, gid_t gid) const {
        return ChownRecursive(uid, gid, RootFilesChownFilter);
    }
    TError ChownRecursiveRootFiles(const TCred &cred) const {
        return ChownRecursiveRootFiles(cred.GetUid(), cred.GetGid());
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

    TError FindMount(TMount &mount, bool exact = false) const;
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
    TError ReadUint64(uint64_t &value) const;

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
private:
    TFile(const TFile&) = delete;
    TFile& operator=(const TFile&) = delete;

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
    void Close(void);
    void Swap(TFile &other);
    static void Close(const std::vector<int> &fds);
    static void CloseAllExcept(const std::vector<int> &except);
    TPath RealPath(void) const;
    TPath ProcPath(void) const;
    TError Read(std::string &text) const;
    TError ReadAll(std::string &text, size_t max) const;
    TError ReadEnds(std::string &text, size_t max) const;
    TError Truncate(off_t size) const;
    TError WriteAll(const std::string &text) const;
    static TError Chattr(int fd, unsigned add_flags, unsigned del_flags);
    int GetMountId(const TPath &relative = "") const;
    TError Dup(const TFile &other);
    TError OpenAt(const TFile &dir, const TPath &path, int flags, int mode = 0);
    TError OpenDirAt(const TFile &dir, const TPath &path);
    TError OpenDirStrictAt(const TFile &dir, const TPath &path);
    TError OpenAtMount(const TFile &mount, const TFile &file, int flags);
    TError OpenDirAllAt(const TFile &dir, const TPath &path);
    TError CreateDirAllAt(const TFile &dir, const TPath &path, int mode, const TCred &cred);
    TError MkdirAt(const TPath &path, int mode) const;
    TError UnlinkAt(const TPath &path) const;
    TError RmdirAt(const TPath &path) const;
    TError RemoveAt(const TPath &path) const;
    TError RenameAt(const TPath &oldpath, const TPath &newpath) const;
    TError HardlinkAt(const TPath &path, const TFile &target, const TPath &target_path = "") const;
    TError SymlinkAt(const TPath &path, const TPath &target) const;
    TError ReadlinkAt(const TPath &path, TPath &target) const;
    TError Chown(uid_t uid, gid_t gid) const;
    TError Chown(const TCred &cred) const {
        return Chown(cred.GetUid(), cred.GetGid());
    }
    TError Chmod(mode_t mode) const;
    TError ChownAt(const TPath &path, uid_t uid, gid_t gid) const;
    TError ChownAt(const TPath &path, const TCred &cred) const {
        return ChownAt(path, cred.GetUid(), cred.GetGid());
    }
    TError ChmodAt(const TPath &path, mode_t mode) const;
    TError Touch() const;
    TError GetXAttr(const std::string &name, std::string &value) const;
    TError SetXAttr(const std::string &name, const std::string &value) const;
    TError Chdir() const;
    TError ClearDirectory() const;
    bool IsRegular() const;
    bool IsDirectory() const;
    TError Stat(struct stat &st) const;
    TError StatAt(const TPath &path, bool follow, struct stat &st) const;
    bool ExistsAt(const TPath &path) const;

    uint32_t FsType() const;
    TError StatFS(TStatFS &result) const;
    TError Chroot() const;
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
private:
    TPathWalk(const TPathWalk&) = delete;
    TPathWalk& operator=(const TPathWalk&) = delete;

public:
    FTS *Fts = nullptr;
    FTSENT *Ent = nullptr;
    TPath Path;
    struct stat *Stat;
    bool Directory = false;
    bool Postorder = false;

    static int CompareNames(const FTSENT **a, const FTSENT **b);
    static int CompareInodes(const FTSENT **a, const FTSENT **b);

    TPathWalk() {}
    ~TPathWalk() { Close(); }
    TError Open(const TPath &path, int fts_flags = FTS_COMFOLLOW | FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, int (*compar)(const FTSENT **, const FTSENT **) = nullptr);
    TError OpenScan(const TPath &path);
    TError OpenList(const TPath &path);
    TError OpenNoStat(const TPath &path);
    TError Next();
    std::string Name() { return Ent ? Ent->fts_name : ""; }
    int Level() { return Ent ? Ent->fts_level : -2; }
    void Close();
};
