#include <string>

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>

#include "error.hpp"

// http://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja

bool WriteDelimitedTo(const google::protobuf::MessageLite& message,
                      google::protobuf::io::ZeroCopyOutputStream* rawOutput);

bool ReadDelimitedFrom(google::protobuf::io::ZeroCopyInputStream* rawInput,
                       google::protobuf::MessageLite* message);

TError ConnectToRpcServer(const std::string& path, int &fd);
TError CreateRpcServer(const std::string &path, const int mode, const int uid, const int gid, int &fd);

class InterruptibleInputStream : public google::protobuf::io::ZeroCopyInputStream {
    int Fd;
    int64_t Pos = 0;
    int64_t Backed = 0;
    std::vector<uint8_t> buf;
    const ssize_t CHUNK_SIZE = 1024;

    void ReserveChunk();

public:
    explicit InterruptibleInputStream(int fd);
    ~InterruptibleInputStream();

    bool Next(const void **data, int *size);
    void BackUp(int count);
    bool Skip(int count);
    int64_t ByteCount() const;
};
