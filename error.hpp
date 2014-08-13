#ifndef __ERROR_HPP__
#define __ERROR_HPP__

#include <string>

#include "rpc.pb.h"

using ::rpc::EError;

class TError {
public:
    TError(EError e = EError::Success, std::string description = "");
    TError(EError e, int err, std::string description = "");

    // return true if non-successful
    operator bool() const;

    int GetError() const;
    const std::string &GetMsg() const;

    static const TError& Success() {
        static TError e;
        return e;
    }

private:
    EError error;
    std::string description;
};

#endif
