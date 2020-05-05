#include "stream.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "client.hpp"
#include "container.hpp"

extern "C" {
#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
}

bool TStdStream::IsNull(void) const {
    return Path.IsEmpty() || Path.ToString() == "/dev/null";
}

/* /dev/fd/%d redirects into client task fd */
bool TStdStream::IsRedirect(void) const {
    return StringStartsWith(Path.ToString(), "/dev/fd/");
}

TPath TStdStream::ResolveOutside(const TContainer &container) const {
    if (IsNull() || IsRedirect())
        return TPath();
    if (Outside) {
        if (Path.IsAbsolute())
            return Path;
        return container.WorkDir() / Path;
    }
    if (Path.IsAbsolute())
        return container.RootPath / Path;
    return container.RootPath / container.GetCwd() / Path;
}

TError TStdStream::Open(const TPath &path, const TCred &cred) {
    int fd, flags;

    Offset = 0;

    if (Stream)
        flags = O_WRONLY | O_APPEND;
    else
        flags = O_RDONLY;

    /* Never assign controlling terminal at open */
    flags |= O_NOCTTY;

retry:
    fd = open(path.c_str(), flags);
    if (fd < 0 && errno == ENOENT && Stream) {
        fd = open(path.c_str(), flags | O_CREAT | O_EXCL, 0660);
        if (fd < 0 && errno == EEXIST)
            goto retry;
        if (fd >= 0 && fchown(fd, cred.GetUid(), cred.GetGid())) {
            close(fd);
            return TError::System("fchown " + path.ToString());
        }
    }
    if (fd < 0)
        return TError(EError::InvalidValue, errno, "open " + path.ToString());

    if (flags != (O_RDWR | O_NOCTTY) && isatty(fd)) {
        close(fd);
        flags = O_RDWR | O_NOCTTY;
        goto retry;
    }

    if (fd != Stream) {
        if (dup2(fd, Stream) < 0) {
            close(fd);
            return TError::System("dup2(" + std::to_string(fd) +
                          ", " + std::to_string(Stream) + ")");
        }
        close(fd);
    } else {
        Stream = fd;
    }

    return OK;
}

TError TStdStream::OpenOutside(const TContainer &container,
                               const TClient &client) {
    if (IsNull())
        return Open("/dev/null", container.TaskCred);

    if (IsRedirect()) {
        int clientFd = -1;
        TError error;

        if (!client.Pid)
            return TError(EError::InvalidValue,
                    "Cannot open redirect without client pid");

        error = StringToInt(Path.ToString().substr(8), clientFd);
        if (error)
            return error;

        TPath path(StringFormat("/proc/%u/fd/%u", client.Pid, clientFd));
        error = Open(path, container.TaskCred);
        if (error)
            return error;

        /* check permissions agains our copy */
        path = StringFormat("/proc/self/fd/%u", Stream);
        struct stat st;
        error = path.StatFollow(st);
        if (error)
            return error;
        if (!TFile::Access(st, client.TaskCred, Stream ? TFile::W : TFile::R) &&
                !TFile::Access(st, client.Cred, Stream ? TFile::W : TFile::R))
            return TError(EError::Permission,
                    "Not enough permissions for redirect: " + Path.ToString());
    } else if (Outside)
        return Open(ResolveOutside(container), container.TaskCred);

    return OK;
}

TError TStdStream::OpenInside(const TContainer &container) {
    TError error;

    if (!Outside && !IsNull() && !IsRedirect())
        error = Open(Path, container.TaskCred);

    /* Assign controlling terminal for our own session */
    if (!error && isatty(Stream))
        (void)ioctl(Stream, TIOCSCTTY, 0);

    return error;
}

TError TStdStream::Remove(const TContainer &container) {
    /* Custom stdout/stderr files are not removed */
    if (!Outside || Path.IsAbsolute())
        return OK;
    TPath path = ResolveOutside(container);
    if (path.IsEmpty() || !path.IsRegularStrict())
        return OK;
    TError error = path.Unlink();
    if (error && error.Errno == ENOENT)
        return OK;
    if (error)
        L_ERR("Cannot remove {}: {}", path, error);
    return error;
}

TError TStdStream::Rotate(const TContainer &container) {
    TPath path = ResolveOutside(container);
    if (path.IsEmpty() || !path.IsRegularStrict())
        return OK;
    off_t loss;
    TError error = path.RotateLog(Limit, loss);
    if (error) {
        Statistics->LogRotateErrors++;
        return error;
    }
    Statistics->LogRotateBytes += loss;
    Offset += loss;
    return OK;
}

TError TStdStream::Read(const TContainer &container, std::string &text,
                        const std::string &range) const {
    std::string off = "", lim = "";
    uint64_t offset, limit;
    TError error;
    TPath path = ResolveOutside(container);

    if (path.IsEmpty())
        return TError(EError::InvalidData, "Data not available");
    if (!path.Exists())
        return TError(EError::InvalidData, "File not found");
    if (!path.IsRegularStrict())
        return TError(EError::InvalidData, "File is non-regular");

    /* [offset][:limit] */
    if (range.size()) {
        auto sep = range.find(':');
        if (sep != std::string::npos) {
            off = range.substr(0, sep);
            lim = range.substr(sep + 1);
        } else
            off = range;
    }

    if (off.size()) {
        error = StringToUint64(off, offset);
        if (error)
            return error;
        if (offset < Offset)
            return TError(EError::InvalidData, "Requested offset lower than current {}", Offset);
        offset -= Offset;
    } else
        offset = 0;

    if (lim.size()) {
        error = StringToUint64(lim, limit);
        if (error)
            return error;
    } else
        limit = Limit;

    TFile file;

    error = file.Open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC);
    if (error)
        return error;

    if (file.RealPath() != path)
        return TError(EError::Permission, "Real path doesn't match: " + path.ToString());

    uint64_t size = lseek(file.Fd, 0, SEEK_END);

    if (size <= offset)
        limit = 0;
    else if (size <= offset + limit)
        limit = size - offset;
    else if (!off.size())
        offset = size - limit;

    const uint64_t stdStreamReadLimit = std::min(Limit, config().container().std_stream_read_limit());
    if (limit > stdStreamReadLimit) {
        if (!off.size())
            offset += limit - stdStreamReadLimit;
        limit = stdStreamReadLimit;
        L_WRN("StdStream read limit exceeded, response truncated");
    }

    if (limit) {
        text.resize(limit);
        ssize_t result = pread(file.Fd, &text[0], limit, offset);

        if (result < 0)
            return TError::System("Read " + path.ToString());

        if ((uint64_t)result < limit)
            text.resize(result);
    }

    return OK;
}
