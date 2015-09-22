#include <string>

#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
}

bool WriteDelimitedTo(
                      const google::protobuf::MessageLite& message,
                      google::protobuf::io::ZeroCopyOutputStream* rawOutput) {
    // We create a new coded stream for each message.  Don't worry, this is fast.
    google::protobuf::io::CodedOutputStream output(rawOutput);

    // Write the size.
    const int size = message.ByteSize();
    output.WriteVarint32(size);
    if (output.HadError())
        return false;

    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != NULL) {
        // Optimization:  The message fits in one buffer, so use the faster
        // direct-to-array serialization path.
        message.SerializeWithCachedSizesToArray(buffer);
    } else {
        // Slightly-slower path when the message is multiple buffers.
        message.SerializeWithCachedSizes(&output);
        if (output.HadError())
            return false;
    }

    return true;
}

bool ReadDelimitedFrom(
                       google::protobuf::io::ZeroCopyInputStream* rawInput,
                       google::protobuf::MessageLite* message) {
    InterruptibleInputStream *is = nullptr;
    try {
        is = dynamic_cast<InterruptibleInputStream *>(rawInput);
    } catch (...) { }

    if (is)
        is->SetLimit(8, false);

    // We create a new coded stream for each message.  Don't worry, this is fast,
    // and it makes sure the 64MB total size limit is imposed per-message rather
    // than on the whole stream.  (See the CodedInputStream interface for more
    // info on this limit.)
    google::protobuf::io::CodedInputStream input(rawInput);

    // Read the size.
    uint32_t size;
    if (!input.ReadVarint32(&size)) return false;

    if (is)
        is->SetLimit(size, true);

    // Tell the stream not to read beyond that size.
    google::protobuf::io::CodedInputStream::Limit limit =
        input.PushLimit(size);

    // Parse the message.
    if (!message->MergeFromCodedStream(&input)) return false;
    if (!input.ConsumedEntireMessage()) return false;

    // Release the limit.
    input.PopLimit(limit);

    return true;
}

TError ConnectToRpcServer(const std::string& path, int &fd)
{
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return TError(EError::Unknown, errno, "socket()");

    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, path.c_str(), sizeof(peer_addr.sun_path) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(fd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0) {
        close(fd);
        fd = -1;
        return TError(EError::Unknown, errno, "connect(" + path + ")");
    }

    return TError::Success();
}

TError CreateRpcServer(const std::string &path, const int mode, const int uid,
                       const int gid, int &fd)
{
    struct sockaddr_un my_addr;

    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return TError(EError::Unknown, errno, "socket()");

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, path.c_str(), sizeof(my_addr.sun_path) - 1);

    (void)unlink(path.c_str());
    if (fchmod(fd, mode) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "fchmod(" + path + ", " + std::to_string(mode) + ")");
    }

    if (::bind(fd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "bind(" + path + ")");
    }

    if (chown(path.c_str(), uid, gid) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "chown(" + path + ", " + std::to_string(uid) + ", " + std::to_string(gid) + ")");
    }

    if (listen(fd, 0) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "listen()");
    }

    return TError::Success();
}

void InterruptibleInputStream::ReserveChunk() {
    if (Pos + CHUNK_SIZE > BufSize) {
        Buf = (uint8_t *)realloc(Buf, Pos + CHUNK_SIZE);
        if (!Buf)
            throw std::bad_alloc();
    }
}

InterruptibleInputStream::InterruptibleInputStream(int fd) : Fd(fd) {
}

InterruptibleInputStream::~InterruptibleInputStream() {
    free(Buf);
}

const uint64_t InterruptibleInputStream::GetLeftovers() const {
    return Leftovers;
}

bool InterruptibleInputStream::Next(const void **data, int *size) {
    int n;

    Leftovers = 0;

    if (Backed) {
        *data = &Buf[Pos - Backed];
        *size = Backed;
        Backed = 0;

        return true;
    }

    ReserveChunk();

    int64_t startPos = Pos;
    int sz = 0;
    while ((n = read(Fd, &Buf[Pos], CHUNK_SIZE)) > 0) {
        Pos += n;
        sz += n;

        if (Limit && sz >= Limit) {
            if (Enforce)
                Leftovers = sz - Limit;
            break;
        }

        if ((size_t)n < CHUNK_SIZE)
            break;

        ReserveChunk();
    }
    if (n < 0 && errno == EINTR)
        interrupted++;

    *data = &Buf[startPos];
    *size = sz;
    return *size != 0;
}

void InterruptibleInputStream::BackUp(int count) {
    Backed = count;
}

bool InterruptibleInputStream::Skip(int count) {
    while (count) {
        uint8_t tmp;
        int n = read(Fd, &tmp, 1);
        if (n != 1)
            return false;
        count--;
    }

    return true;
}

int64_t InterruptibleInputStream::ByteCount() const {
    return Pos - Backed;
}

int InterruptibleInputStream::Interrupted() {
    return interrupted;
}

void InterruptibleInputStream::GetBuf(uint8_t **buf, size_t *pos) const {
    *buf = Buf;
    *pos = Pos;
}

void InterruptibleInputStream::SetLimit(size_t limit, bool enforce) {
    Limit = limit;
    Enforce = enforce;
}
