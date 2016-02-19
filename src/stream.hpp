#pragma once

#include <string>
#include <memory>

#include <util/path.hpp>

class TClient;

class TStdStream {
private:
    int Stream;             /* 0 - stdin, 1 - stdout, 2 - stderr */

    TPath PathOnHost;
    TPath PathInContainer;
    bool ManagedByPorto;

    TError Open(const TPath &path, const TCred &cred) const;

public:
    TStdStream();
    TStdStream(int stream, const TPath &inner_path, const TPath &host_path,
               bool managed_by_porto);

    TError Prepare(const TCred &cred, std::shared_ptr<TClient> client);

    TError OpenOnHost(const TCred &cred) const; // called in child, but host ns
    TError OpenInChild(const TCred &cred) const; // called before actual execve

    TError Rotate(off_t limit, off_t &loss) const;
    TError Cleanup();

    TError Read(std::string &text, off_t limit, uint64_t base,
                const std::string &start_offset = "") const;
};
