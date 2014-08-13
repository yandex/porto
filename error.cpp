#include "error.hpp"

extern "C" {
#include <string.h>
}

TError::TError(EError e, std::string description) :
    error(e), description(description) {
}

TError::TError(EError e, int err, std::string _d) :
    error(e) {
    if (_d.length())
        description = _d;
    else
        description = strerror(err);
}

TError::operator bool() const {
    return error != EError::Success;
}

int TError::GetError() const {
    return error;
}

const std::string &TError::GetMsg() const {
    return description;
}
