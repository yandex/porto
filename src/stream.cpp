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
};

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
        return container.WorkPath() / Path;
    }
    if (Path.IsAbsolute())
        return container.RootPath() / Path;
    return container.RootPath() / container.Cwd / Path;
}

TError TStdStream::Open(const TPath &path, const TCred &cred) {
    int fd, flags;

    Offset = 0;
    Created = false;

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
        Created = true;
        if (fd >= 0 && fchown(fd, cred.Uid, cred.Gid))
            return TError(EError::Unknown, errno, "fchown " + path.ToString());
    }
    if (fd < 0)
        return TError(EError::InvalidValue, errno, "open " + path.ToString());

    if (fd != Stream) {
        if (dup2(fd, Stream) < 0) {
            close(fd);
            return TError(EError::Unknown, errno, "dup2(" + std::to_string(fd) +
                          ", " + std::to_string(Stream) + ")");
        }
        close(fd);
    }

    return TError::Success();
}

TError TStdStream::OpenOutside(const TContainer &container, const TClient &client) {
    if (IsNull())
        return Open("/dev/null", container.OwnerCred);

    if (IsRedirect()) {
        int clientFd = -1;
        TError error;

        error = StringToInt(Path.ToString().substr(8), clientFd);
        if (error)
            return error;

        TPath path(StringFormat("/proc/%u/fd/%u", client.GetPid(), clientFd));
        error = Open(path, container.OwnerCred);
        if (error)
            return error;

        /* check permissions agains our copy */
        path = StringFormat("/proc/self/fd/%u", Stream);
        if (!path.HasAccess(client.TaskCred, Stream ? TPath::W : TPath::R) &&
                !path.HasAccess(client.Cred, Stream ? TPath::W : TPath::R))
            return TError(EError::Permission,
                    "Not enough permissions for redirect: " + Path.ToString());
    } else if (Outside)
        return Open(ResolveOutside(container), container.OwnerCred);

    return TError::Success();
}

TError TStdStream::OpenInside(const TContainer &container) {
    TError error;

    if (!Outside && !IsNull() && !IsRedirect())
        error = Open(Path, container.OwnerCred);

    /* Assign controlling terminal for our own session */
    if (!error && isatty(Stream))
        (void)ioctl(Stream, TIOCSCTTY, 0);

    return error;
}

TError TStdStream::Remove(const TContainer &container) {
    if (!Created)
        return TError::Success();
    TPath path = ResolveOutside(container);
    if (path.IsEmpty() || !path.IsRegularStrict())
        return TError::Success();
    Created = false;
    TError error = path.Unlink();
    if (error)
        L_ERR() << "Cannot remove " << path << " : " << error << std::endl;
    return error;
}

TError TStdStream::Rotate(const TContainer &container) {
    TPath path = ResolveOutside(container);
    if (path.IsEmpty() || !path.IsRegularStrict())
        return TError::Success();
    off_t loss;
    TError error = path.RotateLog(Limit, loss);
    if (error) {
        L_ERR() << "Cannot rotate " << path << " : " << error << std::endl;
        return error;
    }
    Offset += loss;
    return TError::Success();
}

TError TStdStream::Read(const TContainer &container, std::string &text,
                        const std::string &range) const {
    std::string off = "", lim = "";
    uint64_t offset, limit;
    TError error;
    TPath path = ResolveOutside(container);

    if (path.IsEmpty())
        return TError(EError::InvalidData, "data not available");
    if (!path.Exists())
        return TError(EError::InvalidData, "file not found");
    if (!path.IsRegularStrict())
        return TError(EError::InvalidData, "file is non-regular");

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
            return TError(EError::InvalidData, "requested offset lower than current " + std::to_string(Offset));
        offset -= Offset;
    } else
        offset = 0;

    if (lim.size()) {
        error = StringToUint64(lim, limit);
        if (error)
            return error;
    } else
        limit = Limit;

    int fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + path.ToString() + ")");

    uint64_t size = lseek(fd, 0, SEEK_END);

    if (size <= offset)
        limit = 0;
    else if (size <= offset + limit)
        limit = size - offset;
    else if (!off.size())
        offset = size - limit;

    if (limit) {
        text.resize(limit);
        ssize_t result = pread(fd, &text[0], limit, offset);

        if (result < 0)
            error = TError(EError::Unknown, errno, "read " + path.ToString());
        else if ((uint64_t)result < limit)
            text.resize(result);
    }

    close(fd);
    return TError::Success();
}
