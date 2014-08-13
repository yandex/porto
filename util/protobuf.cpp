#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

bool WriteDelimitedTo(
                      const google::protobuf::MessageLite& message,
                      google::protobuf::io::ZeroCopyOutputStream* rawOutput) {
    // We create a new coded stream for each message.  Don't worry, this is fast.
    google::protobuf::io::CodedOutputStream output(rawOutput);

    // Write the size.
    const int size = message.ByteSize();
    output.WriteVarint32(size);

    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != NULL) {
        // Optimization:  The message fits in one buffer, so use the faster
        // direct-to-array serialization path.
        message.SerializeWithCachedSizesToArray(buffer);
    } else {
        // Slightly-slower path when the message is multiple buffers.
        message.SerializeWithCachedSizes(&output);
        if (output.HadError()) return false;
    }

    return true;
}

bool ReadDelimitedFrom(
                       google::protobuf::io::ZeroCopyInputStream* rawInput,
                       google::protobuf::MessageLite* message) {
    // We create a new coded stream for each message.  Don't worry, this is fast,
    // and it makes sure the 64MB total size limit is imposed per-message rather
    // than on the whole stream.  (See the CodedInputStream interface for more
    // info on this limit.)
    google::protobuf::io::CodedInputStream input(rawInput);

    // Read the size.
    uint32_t size;
    if (!input.ReadVarint32(&size)) return false;

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
        return TError(EError::Unknown, errno, "connect(" + path + ")");
    }

    return TError::Success();
}

TError CreateRpcServer(const std::string &path, int &fd)
{
    struct sockaddr_un my_addr;

    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return TError(EError::Unknown, errno, "socket()");

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, path.c_str(), sizeof(my_addr.sun_path) - 1);

    unlink(path.c_str());

    if (bind(fd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "bind(" + path + ")");
    }

    if (listen(fd, 0) < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "listen()");
    }

    return TError::Success();
}
