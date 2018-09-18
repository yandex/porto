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

    TStdStream(int stream): Stream(stream) { }

    void SetOutside(const TString &path) {
        Path = path;
        Outside = true;
    }

    void SetInside(const TString &path) {
        Path = path;
        Outside = false;
    }

    bool IsNull(void) const;
    bool IsRedirect(void) const;
    TPath ResolveOutside(const TContainer &container) const;

    TError Open(const TPath &path, const TCred &cred);
    TError OpenOutside(const TContainer &container, const TClient &client);
    TError OpenInside(const TContainer &container);

    TError Remove(const TContainer &container);

    TError Rotate(const TContainer &container);
    TError Read(const TContainer &container, TString &text,
                const TString &range = "") const;
};
