#include "error.hpp"

extern "C" {
#include <string.h>
}

TError NoError;

TError::TError(EError e, std::string description) :
    error(e), description(description) {
}

TError::TError(EError e, int err) :
    error(e), description(strerror(err)) {
}

TError::operator bool() const {
    return error != NoError;
}

int TError::GetError() const {
    return error;
}

const std::string &TError::GetMsg() const {
    return description;
}
