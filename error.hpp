#pragma once

#include <string>
#include <ostream>

#include "rpc.pb.h"

using ::rpc::EError;

class TError {
    EError Error;
    std::string Description;
    int Errno = 0;

public:
    TError();
    TError(EError e, std::string description, int eno = 0);
    TError(EError e, int eno, std::string description);

    // return true if non-successful
    operator bool() const;

    EError GetError() const;
    std::string GetErrorName() const;
    const std::string &GetMsg() const;
    const int &GetErrno() const;

    static const TError& Success();
    static const TError& Queued();
    friend std::ostream& operator<<(std::ostream& os, const TError& err);

    TError Serialize(int fd) const;
    static bool Deserialize(int fd, TError &error);
};
