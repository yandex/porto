#include <sstream>

#include "path.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <libgen.h>
#include <linux/limits.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <dirent.h>
}

std::string TPath::DirNameStr() const {
    char *dup = strdup(Path.c_str());
    PORTO_ASSERT(dup != nullptr);

    char *p = dirname(dup);
    std::string out(p);
    free(dup);

    return out;
}

TPath TPath::DirName() const {
    std::string s = DirNameStr();
    return TPath(s);
}

std::string TPath::BaseName() const {
    char *dup = strdup(Path.c_str());
    PORTO_ASSERT(dup != nullptr);
    char *p = basename(dup);
    std::string out(p);
    free(dup);

    return out;
}

TError TPath::StatStrict(struct stat &st) const {
    if (lstat(Path.c_str(), &st))
        return TError(EError::Unknown, errno, "lstat " + Path);
    return TError::Success();
}

TError TPath::StatFollow(struct stat &st) const {
    if (stat(Path.c_str(), &st))
        return TError(EError::Unknown, errno, "stat " + Path);
    return TError::Success();
}

bool TPath::IsRegularStrict() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsRegularFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsDirectoryStrict() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISDIR(st.st_mode);
}

bool TPath::IsDirectoryFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISDIR(st.st_mode);
}

unsigned int TPath::GetDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st))
        return 0;

    return st.st_dev;
}

unsigned int TPath::GetBlockDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st) || !S_ISBLK(st.st_mode))
        return 0;

    return st.st_rdev;
}

bool TPath::Exists() const {
    return access(Path.c_str(), F_OK) == 0;
}

bool TPath::HasAccess(const TCred &cred, int mask) const {
    struct stat st;
    int mode;

    if (!cred.Uid && !access(c_str(), mask))
        return true;

    if (stat(c_str(), &st))
        return false;

    if (cred.Uid == st.st_uid)
        mode = st.st_mode >> 6;
    else if (cred.IsMemberOf(st.st_gid))
        mode = st.st_mode >> 3;
    else
        mode = st.st_mode;

    return (mode & mask) == mask;
}

std::string TPath::ToString() const {
    return Path;
}

TPath TPath::AddComponent(const TPath &component) const {
    if (component.IsAbsolute()) {
        if (IsRoot())
            return TPath(component.Path);
        if (component.IsRoot())
            return TPath(Path);
        return TPath(Path + component.Path);
    }
    if (IsRoot())
        return TPath("/" + component.Path);
    return TPath(Path + "/" + component.Path);
}

TError TPath::Chdir() const {
    if (chdir(Path.c_str()) < 0)
        return TError(EError::InvalidValue, errno, "chdir(" + Path + ")");

    return TError::Success();
}

TError TPath::Chroot() const {
    if (chroot(Path.c_str()) < 0)
        return TError(EError::Unknown, errno, "chroot(" + Path + ")");

    return TError::Success();
}

// https://github.com/lxc/lxc/commit/2d489f9e87fa0cccd8a1762680a43eeff2fe1b6e
TError TPath::PivotRoot() const {
    TScopedFd oldroot, newroot;

    oldroot = open("/", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (oldroot.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(/)");

    newroot = open(Path.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (newroot.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    if (fchdir(newroot.GetFd()))
        return TError(EError::Unknown, errno, "fchdir(newroot)");

    if (syscall(__NR_pivot_root, ".", "."))
        return TError(EError::Unknown, errno, "pivot_root()");

    if (fchdir(oldroot.GetFd()) < 0)
        return TError(EError::Unknown, errno, "fchdir(oldroot)");

    if (umount2(".", MNT_DETACH) < 0)
        return TError(EError::Unknown, errno, "umount2(.)");

    if (fchdir(newroot.GetFd()) < 0)
        return TError(EError::Unknown, errno, "fchdir(newroot) reenter");

    return TError::Success();
}

TError TPath::Chown(uid_t uid, gid_t gid) const {
    if (chown(Path.c_str(), uid, gid))
        return TError(EError::Unknown, errno, "chown(" + Path + ", " +
                        UserName(uid) + ", " + GroupName(gid) + ")");
    return TError::Success();
}

TError TPath::Chmod(const int mode) const {
    int ret = chmod(Path.c_str(), mode);
    if (ret)
        return TError(EError::Unknown, errno, "chmod(" + Path + ", " + StringFormat("%#o", mode) + ")");

    return TError::Success();
}

TError TPath::ReadLink(TPath &value) const {
    char buf[PATH_MAX];
    ssize_t len;

    len = readlink(Path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0)
        return TError(EError::Unknown, errno, "readlink(" + Path + ")");

    buf[len] = '\0';

    value = TPath(buf);
    return TError::Success();
}

TError TPath::Symlink(const TPath &target) const {
    int ret = symlink(target.c_str(), Path.c_str());
    if (ret)
        return TError(EError::Unknown, errno, "symlink(" + target.ToString() + ", " + Path + ")");
    return TError::Success();
}

TError TPath::Mknod(unsigned int mode, unsigned int dev) const {
    int ret = mknod(Path.c_str(), mode, dev);
    if (ret)
        return TError(EError::Unknown, errno, "mknod(" + Path + ", " +
                StringFormat("%#o", mode) + ", " + StringFormat("%#x", dev) + ")");
    return TError::Success();
}

TError TPath::Mkfile(unsigned int mode) const {
    return Mknod(S_IFREG | (mode & 0777), 0);
}

TPath TPath::NormalPath() const {
    std::stringstream ss(Path);
    std::string component, path;

    if (IsEmpty())
        return TPath();

    if (IsAbsolute())
        path = "/";

    while (std::getline(ss, component, '/')) {

        if (component == "" || component == ".")
            continue;

        if (component == "..") {
            auto last = path.rfind('/');

            if (last == std::string::npos) {
                /* a/.. */
                if (!path.empty() && path != "..") {
                    path = "";
                    continue;
                }
            } else if (path.compare(last + 1, std::string::npos, "..") != 0) {
                if (last == 0)
                    path.erase(last + 1);   /* /.. or /a/.. */
                else
                    path.erase(last);       /* a/b/.. */
                continue;
            }
        }

        if (!path.empty() && path != "/")
            path += "/";
        path += component;
    }

    if (path.empty())
        path = ".";

    return TPath(path);
}

TPath TPath::AbsolutePath() const {
    char cwd[PATH_MAX];

    if (IsAbsolute() || IsEmpty())
        return TPath(Path);

    if (!getcwd(cwd, sizeof(cwd)))
        return TPath();

    return TPath(std::string(cwd) + "/" + Path);
}

TPath TPath::RealPath() const {
    char *p = realpath(Path.c_str(), NULL);
    if (!p)
        return Path;

    TPath path(p);

    free(p);
    return path;
}

/*
 * Returns relative or absolute path inside this or
 * empty path if argument path is not inside:
 *
 * "/root".InnerPath("/root/foo", true) -> "/foo"
 * "/root".InnerPath("/foo", true) -> ""
 */
TPath TPath::InnerPath(const TPath &path, bool absolute) const {

    unsigned len = Path.length();

    /* check prefix */
    if (!len || path.Path.compare(0, len, Path) != 0)
        return TPath();

    /* exact match */
    if (path.Path.length() == len) {
        if (absolute)
            return TPath("/");
        else
            return TPath(".");
    }

    /* prefix "/" act as "" */
    if (len == 1 && Path[0] == '/')
        len = 0;

    /* '/' must follow prefix */
    if (path.Path[len] != '/')
        return TPath();

    /* cut prefix */
    if (absolute)
        return TPath(path.Path.substr(len));
    else
        return TPath(path.Path.substr(len + 1));
}

TError TPath::StatFS(TStatFS &result) const {
    struct statvfs st;

    int ret = statvfs(Path.c_str(), &st);
    if (ret)
        return TError(EError::Unknown, errno, "statvfs(" + Path + ")");

    result.SpaceUsage = (uint64_t)(st.f_blocks - st.f_bfree) * st.f_bsize;
    result.SpaceAvail = (uint64_t)(st.f_bavail) * st.f_bsize;
    result.InodeUsage = st.f_files - st.f_ffree;
    result.InodeAvail = st.f_favail;
    return TError::Success();
}

TError TPath::Unlink() const {
    if (unlink(c_str()))
        return TError(EError::Unknown, errno, "unlink(" + Path + ")");
    return TError::Success();
}

TError TPath::Rename(const TPath &dest) const {
    if (rename(c_str(), dest.c_str()))
        return TError(EError::Unknown, errno, "rename(" + Path + ", " + dest.Path + ")");
    return TError::Success();
}

TError TPath::Mkdir(unsigned int mode) const {
    if (mkdir(Path.c_str(), mode) < 0)
        return TError(errno == ENOSPC ? EError::NoSpace :
                                        EError::Unknown,
                      errno, "mkdir(" + Path + ", " + StringFormat("%#o", mode) + ")");
    return TError::Success();
}

TError TPath::MkdirAll(unsigned int mode) const {
    std::vector<TPath> paths;
    TPath path(Path);
    TError error;

    while (!path.Exists()) {
        paths.push_back(path);
        path = path.DirName();
    }

    if (!path.IsDirectoryFollow())
        return TError(EError::Unknown, "Not a directory: " + path.ToString());

    for (auto path = paths.rbegin(); path != paths.rend(); path++) {
        error = path->Mkdir(mode);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TPath::MkdirTmp(const TPath &parent, const std::string &prefix, unsigned int mode) {
    Path = (parent / (prefix + "XXXXXX")).Path;
    if (!mkdtemp(&Path[0]))
        return TError(EError::Unknown, errno, "mkdtemp(" + Path + ")");
    if (mode != 0700)
        return Chmod(mode);
    return TError::Success();
}

TError TPath::CreateAll(unsigned int mode) const {
    if (!Exists()) {
        TPath dir = DirName();
        TError error;

        if (!dir.Exists()) {
            error = dir.MkdirAll(0755);
            if (error)
                return error;
        }

        /* This fails for broken symlinks */
        return Mkfile(mode);
    } else if (IsDirectoryFollow())
        return TError(EError::Unknown, "Is a directory: " + Path);

    return TError::Success();
}

TError TPath::Rmdir() const {
    if (rmdir(Path.c_str()) < 0)
        return TError(EError::Unknown, errno, "rmdir(" + Path + ")");
    return TError::Success();
}

/*
 * Removes everything in the directory but not directory itself.
 * Works only on one filesystem and aborts if sees mountpint.
 */
TError TPath::ClearDirectory() const {
    int top_fd, dir_fd, sub_fd;
    DIR *top = NULL, *dir;
    struct dirent *de;
    struct stat top_st, st;
    TError error = TError::Success();

    L_ACT() << "ClearDirectory " << Path << std::endl;

    top_fd = open(Path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                O_NOFOLLOW | O_NOATIME);
    if (top_fd < 0)
        return TError(EError::Unknown, errno, "ClearDirectory open(" + Path + ")");

    if (fstat(top_fd, &top_st)) {
        close(top_fd);
        return TError(EError::Unknown, errno, "ClearDirectory fstat(" + Path + ")");
    }

    dir_fd = top_fd;

deeper:
    dir = fdopendir(dir_fd);
    if (dir == NULL) {
        close(dir_fd);
        if (dir_fd != top_fd)
            closedir(top);
        return TError(EError::Unknown, errno, "ClearDirectory fdopendir(" + Path + "/.../)");
    }

restart:
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (fstatat(dir_fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT)
                continue;
            error = TError(EError::Unknown, errno, "ClearDirectory fstatat(" +
                                          Path + "/.../" + de->d_name + ")");
            break;
        }

        if (st.st_dev != top_st.st_dev) {
            error = TError(EError::Unknown, EXDEV, "ClearDirectory found mountpoint in " + Path);
            break;
        }

        if (Verbose)
            L_ACT() << "ClearDirectory unlink " << de->d_name << std::endl;
        if (!unlinkat(dir_fd, de->d_name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0))
            continue;

        if (errno == ENOENT)
            continue;

        if (errno == EPERM || errno == EACCES) {
            sub_fd = openat(dir_fd, de->d_name, O_RDONLY | O_CLOEXEC |
                                    O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
            if (sub_fd >= 0) {
                error = ChattrFd(sub_fd, 0, FS_APPEND_FL | FS_IMMUTABLE_FL);
                close(sub_fd);
                if (error)
                    L_ERR() << "Cannot change "  << de->d_name <<
                        " attributes: " << error << std::endl;
            }
            error = ChattrFd(dir_fd, 0, FS_APPEND_FL | FS_IMMUTABLE_FL);
            if (error)
                L_ERR() << "Cannot change directory attributes: " << error << std::endl;

            if (!unlinkat(dir_fd, de->d_name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0))
                continue;
        }

        if (!S_ISDIR(st.st_mode) || (errno != ENOTEMPTY && errno != EEXIST)) {
            error = TError(EError::Unknown, errno, "ClearDirectory unlinkat(" +
                           Path + "/.../" + de->d_name + ")");
            break;
        }

        sub_fd = openat(dir_fd, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                            O_NOFOLLOW | O_NOATIME);
        if (sub_fd >= 0) {
            if (dir_fd != top_fd)
                closedir(dir); /* closes dir_fd */
            else
                top = dir;
            dir_fd = sub_fd;
            if (Verbose)
                L_ACT() << "ClearDirectory enter " << de->d_name << std::endl;
            goto deeper;
        }
        if (errno == ENOENT)
            continue;

        error = TError(EError::Unknown, errno, "ClearDirectory openat(" +
                       Path + "/.../" + de->d_name + ")");
        break;
    }

    closedir(dir); /* closes dir_fd */

    if (dir_fd != top_fd) {
        if (!error) {
            rewinddir(top);
            dir = top;
            dir_fd = top_fd;
            if (Verbose)
                L_ACT() << "ClearDirectory restart " << Path << std::endl;
            goto restart; /* Restart from top directory */
        }
        closedir(top); /* closes top_fd */
    }

    return error;
}

TError TPath::RemoveAll() const {
    if (IsDirectoryStrict()) {
        TError error = ClearDirectory();
        if (error)
            return error;
        return Rmdir();
    }
    return Unlink();
}

TError TPath::ReadDirectory(std::vector<std::string> &result) const {
    struct dirent *de;
    DIR *dir;

    result.clear();
    dir = opendir(c_str());
    if (!dir)
        return TError(EError::Unknown, errno, "Cannot open directory " + Path);

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return TError::Success();
}

TError TPath::ListSubdirs(std::vector<std::string> &result) const {
    struct dirent *de;
    DIR *dir;

    result.clear();
    dir = opendir(c_str());
    if (!dir)
        return TError(EError::Unknown, errno, "Cannot open directory " + Path);

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") &&
            (de->d_type == DT_DIR ||
             (de->d_type == DT_UNKNOWN &&
              (*this / std::string(de->d_name)).IsDirectoryStrict())))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return TError::Success();
}

int64_t TPath::SinceModificationMs() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return -1;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 -
           (int64_t)st.st_mtim.tv_sec * 1000 - st.st_mtim.tv_nsec / 1000000;
}

TError TPath::SetXAttr(const std::string name, const std::string value) const {
    if (syscall(SYS_setxattr, Path.c_str(), name.c_str(),
                value.c_str(), value.length(), 0))
        return TError(EError::Unknown, errno,
                "setxattr(" + Path + ", " + name + ")");
    return TError::Success();
}

#ifndef FALLOC_FL_COLLAPSE_RANGE
#define FALLOC_FL_COLLAPSE_RANGE        0x08
#endif

TError TPath::RotateLog(off_t max_disk_usage, off_t &loss) const {
    struct stat st;
    off_t hole_len;
    TError error;
    int fd;

    loss = 0;

    if (lstat(c_str(), &st))
        return TError(EError::Unknown, errno, "lstat(" + Path + ")");

    if (!S_ISREG(st.st_mode) || (off_t)st.st_blocks * 512 <= max_disk_usage)
        return TError::Success();

    fd = open(c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    /* Keep half of allowed size or trucate to zero */
    hole_len = st.st_size - max_disk_usage / 2;
    hole_len -= hole_len % st.st_blksize;
    loss = hole_len;

    if (fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, 0, hole_len)) {
        loss = st.st_size;
        if (ftruncate(fd, 0))
            error = TError(EError::Unknown, errno, "truncate(" + Path + ")");
    }

    close(fd);
    return error;
}

TError TPath::Chattr(unsigned add_flags, unsigned del_flags) const {
    int fd;

    fd = open(c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                       O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");
    TError error = ChattrFd(fd, add_flags, del_flags);
    close(fd);
    return error;
}

#ifndef MS_LAZYTIME
# define MS_LAZYTIME    (1<<25)
#endif

const TFlagsNames TPath::MountFlags = {
    { MS_RDONLY,        "ro" },
    { MS_NOSUID,        "nosuid" },
    { MS_NODEV,         "nodev" },
    { MS_NOEXEC,        "noexec" },
    { MS_SYNCHRONOUS,   "sync" },
    { MS_REMOUNT,       "remount" },
    { MS_MANDLOCK,      "mand" },
    { MS_DIRSYNC,       "dirsync" },
    { MS_NOATIME,       "noatime" },
    { MS_NODIRATIME,    "nodiratime" },
    { MS_BIND,          "bind" },
    { MS_MOVE,          "move" },
    { MS_REC,           "rec" },
    { MS_SILENT,        "silent" },
    { MS_POSIXACL,      "acl" },
    { MS_UNBINDABLE,    "unbindable" },
    { MS_PRIVATE,       "private" },
    { MS_SLAVE,         "slave" },
    { MS_SHARED,        "shared" },
    { MS_RELATIME,      "relatime" },
    { MS_I_VERSION,     "iversion" },
    { MS_STRICTATIME,   "strictatime" },
    { MS_LAZYTIME,      "lazyatime" },
};

const TFlagsNames TPath::UmountFlags = {
    { MNT_FORCE,        "force" },
    { MNT_DETACH,       "detach" },
    { MNT_EXPIRE,       "expire" },
    { UMOUNT_NOFOLLOW,  "nofollow" },
};

std::string TPath::MountFlagsToString(unsigned long flags) {
    return StringFormatFlags(flags, MountFlags);
}

std::string TPath::UmountFlagsToString(unsigned long flags) {
    return StringFormatFlags(flags, UmountFlags);
}

TError TPath::Mount(TPath source, std::string type, unsigned long flags,
                    std::vector<std::string> options) const {
    std::string data = CommaSeparatedList(options);
    L_ACT() << "mount -t " << type << " " << source << " " << Path
            << " -o " << data <<  " " << MountFlagsToString(flags) << std::endl;
    if (mount(source.c_str(), Path.c_str(), type.c_str(), flags, data.c_str()))
        return TError(EError::Unknown, errno, "mount(" + source.ToString() + ", " +
                      Path + ", " + type + ", " + MountFlagsToString(flags) + ", " + data + ")");
    return TError::Success();
}

TError TPath::Bind(TPath source, unsigned long flags) const {
    L_ACT() << "bind mount " << Path << " " << source << " "
            << MountFlagsToString(flags) << std::endl;
    if (mount(source.c_str(), Path.c_str(), NULL, MS_BIND | flags, NULL))
        return TError(EError::Unknown, errno, "mount(" + source.ToString() +
                ", " + Path + ", , " + MountFlagsToString(flags) + ", )");
    if (flags & ~(MS_BIND | MS_REC))
        return Remount(MS_BIND | MS_REMOUNT | flags);
    return TError::Success();
}

TError TPath::Remount(unsigned long flags) const {
    L_ACT() << "remount " << Path << " "
            << MountFlagsToString(flags) << std::endl;
    if (mount(NULL, Path.c_str(), NULL, flags, NULL))
        return TError(EError::Unknown, errno, "mount(NULL, " + Path +
                      ", NULL, " + MountFlagsToString(flags) + ", NULL)");
    return TError::Success();
}

TError TPath::Umount(unsigned long flags) const {
    L_ACT() << "umount " << Path << " "
            << UmountFlagsToString(flags) << std::endl;
    if (umount2(Path.c_str(), flags))
        return TError(EError::Unknown, errno, "umount2(" + Path + ", " +
                                        UmountFlagsToString(flags) +  ")");
    return TError::Success();
}

TError TPath::UmountAll() const {
    TError error = Umount(UMOUNT_NOFOLLOW);
    if (error && error.GetErrno() == EINVAL)
        return TError::Success(); /* not a mountpoint */
    if (error)
        error = Umount(UMOUNT_NOFOLLOW | MNT_DETACH);
    return error;
}

TError TPath::ReadAll(std::string &text, size_t max) const {
    TError error;

    int fd = open(Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return TError(EError::Unknown, errno, "Cannot open for read: " + Path);

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "stat(" + Path + ")");
    }

    if (st.st_size > (off_t)max) {
        close(fd);
        return TError(EError::Unknown, "File too large: " + Path);
    }

    size_t size = st.st_size;
    if (st.st_size < 4096)
        size = 4096;
    text.resize(size);

    size_t off = 0;
    ssize_t ret;
    do {
        if (size - off < 1024) {
            size += 16384;
            if (size > max) {
                error = TError(EError::Unknown, "File too large: " + Path);
                break;
            }
            text.resize(size);
        }
        ret = read(fd, &text[off], size - off);
        if (ret < 0) {
            error = TError(EError::Unknown, errno, "read(" + Path + ")");
            break;
        }
        off += ret;
    } while (ret > 0);

    text.resize(off);
    close(fd);

    return error;
}

TError TPath::WriteAll(const std::string &text) const {
    TError error;

    int fd = open(Path.c_str(), O_WRONLY | O_CLOEXEC | O_TRUNC);
    if (fd < 0)
        return TError(EError::Unknown, errno, "Cannot open for write: " + Path);

    size_t len = text.length(), off = 0;
    do {
        ssize_t ret = write(fd, &text[off], len - off);
        if (ret < 0) {
            error = TError(EError::Unknown, errno, "write(" + Path + ")");
            break;
        }
        off += ret;
    } while (off < len);

    if (close(fd) < 0 && !error)
        error = TError(EError::Unknown, errno, "close(" + Path + ")");

    return error;
}

TError TPath::ReadLines(std::vector<std::string> &lines, size_t max) const {
    char *line = nullptr;
    size_t line_len = 0;
    size_t size = 0;
    ssize_t len;
    struct stat st;
    FILE *file;
    TError error;

    file = fopen(Path.c_str(), "r");
    if (!file)
        return TError(EError::Unknown, errno, "Cannot open for read: " + Path);

    if (fstat(fileno(file), &st) < 0) {
        error = TError(EError::Unknown, errno, "stat(" + Path + ")");
        goto out;
    }

    if (st.st_size > (off_t)max) {
        error = TError(EError::Unknown, "File too large: " + Path);
        goto out;
    }

    while ((len = getline(&line, &line_len, file)) >= 0) {
        size += len;
        if (size > max) {
            error = TError(EError::Unknown, "File too large: " + Path);
            goto out;
        }
        lines.push_back(std::string(line, len - 1));
    }

out:
    fclose(file);
    free(line);

    return error;
}

TError TPath::ReadInt(int &value) const {
    std::string text;
    TError error = ReadAll(text);
    if (!error)
        error = StringToInt(text, value);
    return error;
}
