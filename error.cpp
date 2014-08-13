#include "error.hpp"

extern "C" {
#include <string.h>
}

TError::TError() : error(EError::Success) {
}

TError::TError(EError e, std::string description) :
    error(e), description(description) {
}

TError::TError(EError e, int eno, std::string description) :
    error(e), description(std::string(strerror(eno)) + ": " + description) {
}

TError::operator bool() const {
    return error != EError::Success;
}

EError TError::GetError() const {
    return error;
}

const std::string &TError::GetMsg() const {
    return description;
}
