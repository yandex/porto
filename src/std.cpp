#include "std.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/file.hpp"

extern "C" {
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
};

TStdStream::TStdStream() {
}

TStdStream::TStdStream(int type, const std::string &impl,
                       const TPath &inner_path, const TPath &host_path,
                       bool managed_by_porto) :
    Type(type), Impl(impl), PathOnHost(host_path), PathInContainer(inner_path),
    ManagedByPorto(managed_by_porto) {}

TError TStdStream::Prepare(const TCred &cred) {
    if (Impl == STD_TYPE_FIFO) {
        if (mkfifo(PathOnHost.ToString().c_str(), 0600)) {
            return TError(EError::Unknown, errno, "mkfifo()");
        }

        int ret = chown(PathOnHost.ToString().c_str(), cred.Uid, cred.Gid);
        if (ret < 0)
            return TError(EError::Unknown, errno, "fchown(" + PathOnHost.ToString() + ")");

        int flags = O_NONBLOCK | O_RDWR;
        PipeFd = open(PathOnHost.ToString().c_str(), flags, 0660);
        if (PipeFd < 0)
            return TError(EError::InvalidValue, errno,
                          "open(" + PathOnHost.ToString() + ") -> " +
                          std::to_string(PipeFd));

        flags = fcntl(PipeFd, F_GETFL, 0);
        if (flags == -1)
            return TError(EError::Unknown, errno, "fcntl");

        auto res = fcntl(PipeFd, F_SETFL, flags | O_NONBLOCK);
        if (res == -1)
            return TError(EError::Unknown, errno, "fcntl");
    }
    return TError::Success();
}

TError TStdStream::Open(const TPath &path, const TCred &cred) const {
    if (Impl == STD_TYPE_FILE || Impl == STD_TYPE_FIFO) {
        int flags;
        if (Impl == STD_TYPE_FIFO)
            flags = Type ? O_WRONLY : O_RDONLY;
        else
            flags = Type ? (O_CREAT | O_WRONLY | O_APPEND) : O_RDONLY;

        int ret = open(path.ToString().c_str(), flags, 0660);
        if (ret < 0)
            return TError(EError::InvalidValue, errno,
                          "open(" + path.ToString() + ") -> " +
                          std::to_string(ret));
        if (ret != Type) {
            if (dup2(ret, Type) < 0) {
                close(ret);
                return TError(EError::Unknown, errno, "dup2(" + std::to_string(ret) +
                              ", " + std::to_string(Type) + ")");
            }
            close(ret);
            ret = Type;
        }

        if (Impl == STD_TYPE_FILE && path.IsRegular()) {
            ret = fchown(ret, cred.Uid, cred.Gid);
            if (ret < 0)
                return TError(EError::Unknown, errno, "fchown(" + path.ToString() + ")");
        }
    }
    return TError::Success();
}

TError TStdStream::OpenOnHost(const TCred &cred) const {
    if (ManagedByPorto && (Impl == STD_TYPE_FILE || Impl == STD_TYPE_FIFO))
        return Open(PathOnHost, cred);
    else
        return TError::Success();
}

TError TStdStream::OpenInChild(const TCred &cred) const {
    if (!ManagedByPorto && (Impl == STD_TYPE_FILE || Impl == STD_TYPE_FIFO))
        return Open(PathInContainer, cred);
    else
        return TError::Success();
}

TError TStdStream::Rotate(off_t limit, off_t &loss) const {
    if (Impl == STD_TYPE_FILE && PathOnHost.IsRegular()) {
        return PathOnHost.RotateLog(config().container().max_log_size(), loss);
    } else
        return TError::Success();
}

TError TStdStream::Cleanup() const {
    if (ManagedByPorto && Impl == STD_TYPE_FILE && PathOnHost.IsRegular() && Type) {
        TError err = PathOnHost.Unlink();
        if (err)
            L_ERR() << "Can't remove std log: " << err << std::endl;
        return err;
    } else if (Impl == STD_TYPE_FIFO) {
        close(PipeFd);
        TError err = PathOnHost.Unlink();
        if (err)
            L_ERR() << "Can't remove fifo: " << err << std::endl;
        return err;
    }

    return TError::Success();
}

TError TStdStream::Read(std::string &text, off_t limit, uint64_t base, const std::string &start_offset) const {
    if (Impl == STD_TYPE_FILE) {
        uint64_t offset = 0;
        TError error;

        if (!PathOnHost.IsRegular())
            return TError(EError::InvalidData, "file is non-regular");

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
    } else if (Impl == STD_TYPE_FIFO) {
            char buf[limit];

            auto len = read(PipeFd, buf, sizeof(buf));
            if (len < 0)
                return TError(EError::Unknown, errno, "read(" + PathOnHost.ToString() + ")");
            if (len > 0)
                text = std::string(buf, len);

    }
    return TError::Success();
}
