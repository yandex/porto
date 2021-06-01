#pragma once

#include <string>
#include <util/path.hpp>

class TContainer;
class TClient;

class TStdStream {
public:
    int Stream;             /* 0 - stdin, 1 - sstdout, 2 - stderr */
    TPath Path;
    bool Outside = false;
    uint64_t Limit = 0;
    uint64_t Offset = 0;
    struct stat PathStat = {};

    TStdStream(int stream): Stream(stream) { }

    void SetOutside(const std::string &path) {
        Path = path;
        Outside = true;
    }

    TError SetInside(const std::string &path, const TClient &client, bool restore = false);
    bool IsNull(void) const;
    bool IsRedirect(void) const;
    TPath ResolveOutside(const TContainer &container) const;

    TError Open(const TPath &path, const TCred &cred);
    TError OpenOutside(const TContainer &container, const TClient &client);
    TError OpenInside(const TContainer &container);

    TError Remove(const TContainer &container);

    TError Rotate(const TContainer &container);
    TError Read(const TContainer &container, std::string &text,
                const std::string &range = "") const;
};
