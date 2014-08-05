#ifndef __ERROR_HPP__
#define __ERROR_HPP__

#include <string>

class TError {
    int error;
    std::string msg;

public:
    TError();
    TError(const std::string &msg);
    TError(int error, const std::string &msg = "");

    // return true if non-successful
    operator bool() const;

    int GetError();
    const std::string &GetMsg();
};

#endif
