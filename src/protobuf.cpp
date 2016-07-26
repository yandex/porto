#include <string>

#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
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
