#pragma once

#include <string>
#include <ostream>
#include "fmt/ostream.h"

#include "rpc.pb.h"

using ::rpc::EError;

class TError {
    EError Error;
    std::string Description;
    int Errno = 0;

public:
    TError();
    TError(EError e, const std::string &description, int eno = 0);
    TError(EError e, int eno, const std::string &description);

    TError(EError e, std::string &&description, int eno = 0);
    TError(const TError &other) = default;
    TError(TError &&other) = default;
    TError(const TError &other, const std::string &prefix);
    TError &operator=(TError &&other) = default;
    TError &operator=(const TError &other) = default;

    // return true if non-successful
    explicit operator bool() const;

    EError GetError() const;
    std::string GetErrorName() const;
    const std::string &GetMsg() const;
    const int &GetErrno() const;

    static const TError& Success();
    static const TError& Queued();
    static TError FromErrno(const std::string &description);
    friend std::ostream& operator<<(std::ostream& os, const TError& err);

    template<typename ostream>
    friend ostream& operator<<(ostream& os, const TError &err) {
        os << err.GetErrorName() << " (" << err.GetMsg() << ")";
        return os;
    }

    TError Serialize(int fd) const;
    static bool Deserialize(int fd, TError &error);
};

inline bool operator==(const TError &lhs, const TError &rhs) {
    return lhs.GetError() == rhs.GetError() && lhs.GetErrno() == rhs.GetErrno();
}

inline bool operator!=(const TError &lhs, const TError &rhs) {
    return lhs.GetError() != rhs.GetError() || lhs.GetErrno() != rhs.GetErrno();
}
