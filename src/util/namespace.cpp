#include "namespace.hpp"
#include "log.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}

TError TNamespaceFd::Open(TPath path) {
    Close();
    Fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (Fd < 0)
        return TError(EError::Unknown, errno, "Cannot open " + path.ToString());
    PORTO_ASSERT(Fd > 2);
    return TError::Success();
}

TError TNamespaceFd::Open(pid_t pid, std::string type) {
    return Open("/proc/" + std::to_string(pid) + "/" + type);
}

void TNamespaceFd::Close() {
    if (Fd >= 0) {
        PORTO_ASSERT(Fd > 2);

        int ret = close(Fd);
        PORTO_ASSERT(!ret);

        Fd = -1;
    }
}

TError TNamespaceFd::SetNs(int type) const {
    if (Fd >= 0 && setns(Fd, type))
        return TError(EError::Unknown, errno, "Cannot set namespace");
    return TError::Success();
}

TError TNamespaceFd::Chdir() const {
    if (Fd >= 0 && fchdir(Fd))
        return TError(EError::Unknown, errno, "Cannot change cwd");
    return TError::Success();
}

TError TNamespaceFd::Chroot() const {
    if (Fd >= 0) {
        int ret = fchdir(Fd);
        if (!ret)
            ret = chroot(".");

        if (ret)
            return TError(EError::Unknown, errno, "Cannot change root");
    }

    return TError::Success();
}

ino_t TNamespaceFd::Inode() const {
    struct stat st;
    if (Fd > 0 && fstat(Fd, &st) == 0)
        return st.st_ino;
    return -1;
}

ino_t TNamespaceFd::PidInode(pid_t pid, std::string type) {
    struct stat st;
    if (TPath("/proc/" + std::to_string(pid) + "/" + type).StatFollow(st))
        return 0;
    return st.st_ino;
}
