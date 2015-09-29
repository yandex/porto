#pragma once

#include <string>

#include <google/protobuf/io/zero_copy_stream.h>

#include "error.hpp"

// http://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja

bool WriteDelimitedTo(const google::protobuf::MessageLite& message,
                      google::protobuf::io::ZeroCopyOutputStream* rawOutput);
bool WriteDelimitedTo(const google::protobuf::MessageLite& message, int fd, bool flush = false);

bool ReadDelimitedFrom(google::protobuf::io::ZeroCopyInputStream* rawInput,
                       google::protobuf::MessageLite* message);

TError ConnectToRpcServer(const std::string& path, int &fd);
TError CreateRpcServer(const std::string &path, const int mode, const int uid,
                       const int gid, int &fd);

class InterruptibleInputStream final : public google::protobuf::io::ZeroCopyInputStream {
    static const size_t CHUNK_SIZE = 1024;

    int Fd;
    size_t Pos = 0;
    int64_t Backed = 0;
    uint8_t *Buf = nullptr;
    size_t BufSize = 0;
    int InterruptedCount = 0;
    int Limit = 0;
    uint64_t Leftovers = 0;
    bool Enforce = false;

    void ReserveChunk();

public:
    explicit InterruptibleInputStream(int fd);
    ~InterruptibleInputStream() final;

    // ZeroCopyInputStream implementation.
    bool Next(const void **data, int *size) final;
    void BackUp(int count) final;
    bool Skip(int count) final;
    int64_t ByteCount() const final;

    int Interrupted() const;
    void GetBuf(uint8_t **buf, size_t *pos) const;
    void SetLimit(size_t limit, bool enforce);
    const uint64_t GetLeftovers() const;
};
