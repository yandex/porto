#pragma once

#include <string>

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>

#include "error.hpp"

// http://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja

class InterruptibleInputStream;

bool WriteDelimitedTo(const google::protobuf::MessageLite& message,
                      google::protobuf::io::ZeroCopyOutputStream* rawOutput);

bool ReadDelimitedFrom(google::protobuf::io::ZeroCopyInputStream* rawInput,
                       google::protobuf::MessageLite* message);

TError ConnectToRpcServer(const std::string& path, int &fd);
TError CreateRpcServer(const std::string &path, const int mode, const int uid,
                       const int gid, int &fd);

class InterruptibleInputStream : public google::protobuf::io::ZeroCopyInputStream {
    int Fd;
    size_t Pos = 0;
    int64_t Backed = 0;
    uint8_t *Buf = nullptr;
    size_t BufSize = 0;
    const size_t CHUNK_SIZE = 1024;
    int interrupted = false;
    int Limit = 0;
    uint64_t Leftovers = 0;
    bool Enforce = false;

    void ReserveChunk();

public:
    explicit InterruptibleInputStream(int fd);
    ~InterruptibleInputStream();

    bool Next(const void **data, int *size);
    void BackUp(int count);
    bool Skip(int count);
    int64_t ByteCount() const;
    int Interrupted();
    void GetBuf(uint8_t **buf, size_t *pos) const;
    void SetLimit(size_t limit, bool enforce);
    const uint64_t GetLeftovers() const;
};
