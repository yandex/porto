#include "stream.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "client.hpp"

extern "C" {
#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
};

TStdStream::TStdStream() {
}

TStdStream::TStdStream(int stream,
                       const TPath &inner_path, const TPath &host_path,
                       bool managed_by_porto) :
    Stream(stream),
    PathOnHost(host_path), PathInContainer(inner_path),
    ManagedByPorto(managed_by_porto)
{
    if (PathInContainer == "/dev/null") {
        PathOnHost = "/dev/null";
        ManagedByPorto = true;
    }

    if (StringStartsWith(PathInContainer.ToString(), "/dev/fd/"))
        ManagedByPorto = true;
}

TError TStdStream::Prepare(const TCred &cred, std::shared_ptr<TClient> client) {
    int clientFd = -1;

    if (StringStartsWith(PathInContainer.ToString(), "/dev/fd/") &&
            StringToInt(PathInContainer.ToString().substr(8), clientFd))
        return TError(EError::InvalidValue, "invalid std path: " +
                                            PathInContainer.ToString());

    if (clientFd >= 0) {
        if (client) {
            TPath fd(StringFormat("/proc/%u/fd/%u", client->GetPid(), clientFd));
            if (!fd.HasAccess(client->TaskCred, Stream ? TPath::W : TPath::R) &&
                    !fd.HasAccess(client->Cred, Stream ? TPath::W : TPath::R))
                return TError(EError::Permission,
                              std::string("client have no ") +
                              (Stream ? "write" : "read") +
                              " access to " + PathInContainer.ToString());
            PathOnHost = fd;
        } else
            PathOnHost = "/dev/null";
    }

    return TError::Success();
}

TError TStdStream::Open(const TPath &path, const TCred &cred) const {
    int flags = O_RDONLY;

    if (Stream != STDIN_FILENO)
        flags = O_WRONLY | O_CREAT | O_APPEND;

    /* Never assign controlling terminal at open */
    flags |= O_NOCTTY;

    int ret = open(path.ToString().c_str(), flags, 0660);
    if (ret < 0)
        return TError(EError::InvalidValue, errno,
                      "open(" + path.ToString() + ") -> " +
                      std::to_string(ret));

    if (ret != Stream) {
        if (dup2(ret, Stream) < 0) {
            close(ret);
            return TError(EError::Unknown, errno, "dup2(" + std::to_string(ret) +
                          ", " + std::to_string(Stream) + ")");
        }
        close(ret);
        ret = Stream;
    }

    if (path.IsRegularStrict()) {
        ret = fchown(ret, cred.Uid, cred.Gid);
        if (ret < 0)
            return TError(EError::Unknown, errno, "fchown(" + path.ToString() + ")");
    }

    return TError::Success();
}

TError TStdStream::OpenOnHost(const TCred &cred) const {
    if (ManagedByPorto)
        return Open(PathOnHost, cred);
    return TError::Success();
}

TError TStdStream::OpenInChild(const TCred &cred) const {
    TError error;

    if (!ManagedByPorto)
        error = Open(PathInContainer, cred);

    /* Assign controlling terminal for our own session */
    if (!error && isatty(Stream))
        (void)ioctl(Stream, TIOCSCTTY, 0);

    return error;
}

TError TStdStream::Rotate(off_t limit, off_t &loss) const {
    loss = 0;
    if (PathOnHost.IsRegularStrict())
        return PathOnHost.RotateLog(config().container().max_log_size(), loss);
    return TError::Success();
}

TError TStdStream::Cleanup() {
    if (ManagedByPorto && PathOnHost.IsRegularStrict() && Stream) {
        TError err = PathOnHost.Unlink();
        if (err)
            L_ERR() << "Can't remove std log: " << err << std::endl;
        return err;
    }

    return TError::Success();
}

TError TStdStream::Read(std::string &text, off_t limit, uint64_t base, const std::string &start_offset) const {
    uint64_t offset = 0;
    TError error;

    if (!PathOnHost.IsRegularStrict()) {
        if (!PathOnHost.Exists())
            return TError(EError::InvalidData, "file not found");
        return TError(EError::InvalidData, "file is non-regular");
    }

    if (start_offset != "") {
        TError error = StringToUint64(start_offset, offset);
        if (error)
            return error;

        if (offset < base)
            return TError(EError::InvalidData,
                    "requested offset lower than current " + std::to_string(base));
        offset -= base;
    }

    int fd = open(PathOnHost.c_str(), O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + PathOnHost.ToString() + ")");

    uint64_t size = lseek(fd, 0, SEEK_END);

    if (size <= offset)
        limit = 0;
    else if (size <= offset + limit)
        limit = size - offset;
    else if (start_offset == "")
        offset = size - limit;

    if (limit) {
        text.resize(limit);
        auto result = pread(fd, &text[0], limit, offset);

        if (result < 0)
            error = TError(EError::Unknown, errno, "read(" + PathOnHost.ToString() + ")");
        else if (result < limit)
            text.resize(result);
    }

    close(fd);
    return TError::Success();
}
