#include "path.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <libgen.h>
#include <linux/limits.h>
#include <sys/syscall.h>
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

std::string TPath::BaseName() {
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
    return StringRemoveRepeating(Path, '/');
}

TPath TPath::AddComponent(const TPath &component) const {
    return TPath(Path + "/" + component.ToString());
}

TError TPath::Chdir() const {
    if (chdir(Path.c_str()) < 0)
        return TError(EError::Unknown, errno, "chdir(" + Path + ")");

    return TError::Success();
}

TError TPath::Chroot() const {
    if (chroot(Path.c_str()) < 0)
        return TError(EError::Unknown, errno, "chroot(" + Path + ")");

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

TError TPath::Symlink(const TPath &to) const {
    TPath target;
    TError error = ReadLink(target);
    if (error)
        return error;

    int ret = symlink(target.ToString().c_str(), to.ToString().c_str());
    if (ret)
        return TError(EError::Unknown, errno, "symlink(" + Path + ", " + to.ToString() + ")");
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

TError TPath::Copy(const TPath &to) const {
    switch(GetType()) {
    case EFileType::Directory:
        return TError(EError::Unknown, "Can't copy directory " + Path);
    case EFileType::Regular:
        return RegularCopy(to, GetMode());
    case EFileType::Block:
        return to.Mkfifo(GetMode());
    case EFileType::Socket:
    case EFileType::Character:
    case EFileType::Fifo:
        return to.Mknod(GetMode(), GetDev());
    case EFileType::Link:
        return Symlink(to);
    case EFileType::Unknown:
    case EFileType::Any:
        break;
    }
    return TError(EError::Unknown, "Unknown file type " + Path);
}
