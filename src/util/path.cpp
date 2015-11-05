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

using std::string;

std::string AccessTypeToString(EFileAccess type) {
    if (type == EFileAccess::Read)
        return "read";
    else if (type == EFileAccess::Write)
        return "write";
    else if (type == EFileAccess::Execute)
        return "execute";
    else
        return "unknown";
}

std::string TPath::DirNameStr() const {
    char *dup = strdup(Path.c_str());
    if (!dup)
        throw std::bad_alloc();

    char *p = dirname(dup);
    std::string out(p);
    free(dup);

    return out;
}

TPath TPath::DirName() const {
    string s = DirNameStr();
    return TPath(s);
}

std::string TPath::BaseName() const {
    char *dup = strdup(Path.c_str());
    if (!dup)
        throw std::bad_alloc();

    char *p = basename(dup);
    std::string out(p);
    free(dup);

    return out;
}

EFileType TPath::GetType() const {
    struct stat st;

    if (!Path.length())
        return EFileType::Unknown;

    if (lstat(Path.c_str(), &st))
        return EFileType::Unknown;

    if (S_ISREG(st.st_mode))
        return EFileType::Regular;
    else if (S_ISDIR(st.st_mode))
        return EFileType::Directory;
    else if (S_ISCHR(st.st_mode))
        return EFileType::Character;
    else if (S_ISBLK(st.st_mode))
        return EFileType::Block;
    else if (S_ISFIFO(st.st_mode))
        return EFileType::Fifo;
    else if (S_ISLNK(st.st_mode))
        return EFileType::Link;
    else if (S_ISSOCK(st.st_mode))
        return EFileType::Socket;
    else
        return EFileType::Unknown;
}

unsigned int TPath::Stat(std::function<unsigned int(struct stat *st)> f) const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return 0;

    return f(&st);
}

unsigned int TPath::GetMode() const {
    return Stat([](struct stat *st) { return st->st_mode & 0x1FF; });
}

unsigned int TPath::GetDev() const {
    return Stat([](struct stat *st) { return st->st_dev; });
}

/* Follows symlinks */
unsigned int TPath::GetBlockDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st) || !S_ISBLK(st.st_mode))
        return 0;

    return st.st_rdev;
}

unsigned int TPath::GetUid() const {
    return Stat([](struct stat *st) { return st->st_uid; });
}

unsigned int TPath::GetGid() const {
    return Stat([](struct stat *st) { return st->st_gid; });
}

off_t TPath::GetSize() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return -1;

    return st.st_size;
}

off_t TPath::GetDiskUsage() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return -1;

    return st.st_blocks * 512;
}

bool TPath::IsRegular() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsDirectory() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISDIR(st.st_mode);
}

bool TPath::Exists() const {
    return access(Path.c_str(), F_OK) == 0;
}

bool TPath::AccessOk(EFileAccess type) const {
    switch (type) {
    case EFileAccess::Read:
        return access(Path.c_str(), R_OK) == 0;
    case EFileAccess::Write:
        return access(Path.c_str(), W_OK) == 0;
    case EFileAccess::Execute:
        return access(Path.c_str(), X_OK) == 0;
    default:
        return false;
    }
}

bool TPath::AccessOk(EFileAccess type, const TCred &cred) const {
    int ret;
    bool result = false;

    /*
     * Set real uid/gid -- syscall access uses it for checks.
     * Use raw syscalls because libc wrapper changes uid for all threads.
     */
    ret = syscall(SYS_setreuid, cred.Uid, 0);
    if (ret)
        goto exit;
    ret = syscall(SYS_setregid, cred.Gid, 0);
    if (ret)
        goto exit;

    result = AccessOk(type);

exit:
    (void)syscall(SYS_setreuid, 0, 0);
    (void)syscall(SYS_setregid, 0, 0);

    return result;
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

TError TPath::Chown(const TCred &cred) const {
    return Chown(cred.UserAsString(), cred.GroupAsString());
}

TError TPath::Chown(unsigned int uid, unsigned int gid) const {
    int ret = chown(Path.c_str(), uid, gid);
    if (ret)
        return TError(EError::Unknown, errno, "chown(" + Path + ", " + std::to_string(uid) + ", " + std::to_string(gid) + ")");

    return TError::Success();
}

TError TPath::Chown(const std::string &user, const std::string &group) const {
    TCred cred;

    TError error = cred.Parse(user, group);
    if (error)
        return error;

    return Chown(cred.Uid, cred.Gid);
}

TError TPath::Chmod(const int mode) const {
    int ret = chmod(Path.c_str(), mode);
    if (ret)
        return TError(EError::Unknown, errno, "chmod(" + Path + ", " + std::to_string(mode) + ")");

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

TError TPath::Mkfifo(unsigned int mode) const {
    int ret = mkfifo(Path.c_str(), mode);
    if (ret)
        return TError(EError::Unknown, errno, "mkfifo(" + Path + ", " + std::to_string(mode) + ")");
    return TError::Success();
}

TError TPath::Mknod(unsigned int mode, unsigned int dev) const {
    int ret = mknod(Path.c_str(), mode, dev);
    if (ret)
        return TError(EError::Unknown, errno, "mknod(" + Path + ", " + std::to_string(mode) + ", " + std::to_string(dev) + ")");
    return TError::Success();
}

TError TPath::RegularCopy(const TPath &to, unsigned int mode) const {
    TScopedFd in, out;

    in = open(Path.c_str(), O_RDONLY);
    if (in.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");
    out = creat(to.ToString().c_str(), mode);
    if (out.GetFd() < 0)
        return TError(EError::Unknown, errno, "creat(" + to.ToString() + ")");

    int n, ret;
    char buf[4096];
    while ((n = read(in.GetFd(), buf, sizeof(buf))) > 0) {
        ret = write(out.GetFd(), buf, n);
        if (ret != n)
            return TError(EError::Unknown, errno, "partial copy(" + Path + ", " + to.ToString() + ")");
    }

    if (n < 0)
        return TError(EError::Unknown, errno, "copy(" + Path + ", " + to.ToString() + ")");

    return TError::Success();
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

TError TPath::StatVFS(uint64_t &space_used, uint64_t &space_avail,
                      uint64_t &inode_used, uint64_t &inode_avail) const {
    struct statvfs st;

    int ret = statvfs(Path.c_str(), &st);
    if (ret)
        return TError(EError::Unknown, errno, "statvfs(" + Path + ")");

    space_used = (uint64_t)(st.f_blocks - st.f_bfree) * st.f_bsize;
    space_avail = (uint64_t)(st.f_bavail) * st.f_bsize;
    inode_used = st.f_files - st.f_ffree;
    inode_avail = st.f_favail;
    return TError::Success();
}

TError TPath::StatVFS(uint64_t &space_avail) const {
    uint64_t space_used, inode_used, inode_avail;
    return StatVFS(space_used, space_avail, inode_used, inode_avail);
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
                      errno, "mkdir(" + Path + ", " + std::to_string(mode) + ")");
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

    for (auto path = paths.rbegin(); path != paths.rend(); path++) {
        error = path->Mkdir(mode);
        if (error)
            return error;
    }

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
TError TPath::ClearDirectory(bool verbose) const {
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

        if (verbose)
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
            if (verbose)
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
            if (verbose)
                L_ACT() << "ClearDirectory restart " << Path << std::endl;
            goto restart; /* Restart from top directory */
        }
        closedir(top); /* closes top_fd */
    }

    return error;
}

TError TPath::RemoveAll() const {
    if (IsDirectory()) {
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
            result.push_back(string(de->d_name));
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

TError TPath::RotateLog(off_t max_disk_usage) const {
    struct stat st;
    off_t hole_len;
    TError error;
    int fd;

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

    if (fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, 0, hole_len) &&
            ftruncate(fd, 0))
        error = TError(EError::Unknown, errno, "truncate(" + Path + ")");
    else
        error = TError::Success();

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
