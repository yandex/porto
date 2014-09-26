#include "error.hpp"

extern "C" {
#include <string.h>
}

TError::TError() : Error(EError::Success) {
}

TError::TError(EError e, std::string description) :
    Error(e), Description(description) {
}

TError::TError(EError e, int eno, std::string description) :
    Error(e), Description(std::string(strerror(eno)) + ": " + description), Errno(eno) {
}

TError::operator bool() const {
    return Error != EError::Success;
}

EError TError::GetError() const {
    return Error;
}

std::string TError::GetErrorName() const {
    return rpc::EError_Name(Error);
}

const std::string &TError::GetMsg() const {
    return Description;
}

const int &TError::GetErrno() const {
    return Errno;
}
