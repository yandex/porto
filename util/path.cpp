#include "path.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <libgen.h>
#include <linux/limits.h>
}

using std::string;

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

unsigned int TPath::GetMode() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return 0;

    return st.st_mode & 0x1FF;
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

std::string TPath::ToString() const {
    return StringRemoveRepeating(Path, '/');
}

void TPath::AddComponent(const std::string &component) {
    Path += "/";
    Path += component;
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

TError TPath::Chown(const std::string &user, const std::string &group) const {
    int uid, gid;

    TUser u(user);
    TError error = u.Load();
    if (error)
        return error;

    uid = u.GetId();

    TGroup g(group);
    error = g.Load();
    if (error)
        return error;

    gid = g.GetId();

    int ret = chown(Path.c_str(), uid, gid);
    if (ret)
        return TError(EError::Unknown, errno, "chown(" + Path + ", " + user + ", " + group + ")");

    return TError::Success();
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
