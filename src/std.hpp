#pragma once

#include <string>
#include <memory>

#include <util/path.hpp>

constexpr const char* STD_TYPE_FILE = "file";
constexpr const char* STD_TYPE_FIFO = "fifo";
constexpr const char* STD_TYPE_PTY = "pty";

class TClient;

class TStdStream {
private:
    int Stream;             /* 0 - stdin, 1 - stdout, 2 - stderr */
    std::string Type;       /* file, pipe, pty */

    TPath PathOnHost;
    TPath PathInContainer;
    bool ManagedByPorto;
    int PipeFd = -1;

    TError Open(const TPath &path, const TCred &cred) const;

public:
    TStdStream();
    TStdStream(int stream, const std::string &type,
               const TPath &inner_path, const TPath &host_path,
               bool managed_by_porto);

    TError Prepare(const TCred &cred, std::shared_ptr<TClient> client);

    TError OpenOnHost(const TCred &cred) const; // called in child, but host ns
    TError OpenInChild(const TCred &cred) const; // called before actual execve

    TError Rotate(off_t limit, off_t &loss) const;
    TError Cleanup();
    TError Close();

    TError Read(std::string &text, off_t limit, uint64_t base,
                const std::string &start_offset = "") const;
};
