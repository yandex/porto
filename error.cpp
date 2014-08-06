#include "error.hpp"

extern "C" {
#include <string.h>
}

TError::TError() : error(0) {
}

TError::TError(const std::string &msg) : error(-1), msg(msg) {
}

TError::TError(int error, const std::string &_msg) : error(error) {
    if (!_msg.length())
        msg = std::string(strerror(error));
}

TError::operator bool() const {
    return error != 0;
}

int TError::GetError() const {
    return error;
}

const std::string &TError::GetMsg() const {
    return msg;
}
