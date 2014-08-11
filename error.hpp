#ifndef __ERROR_HPP__
#define __ERROR_HPP__

#include <string>

class TError {
public:
    enum EError {
        NoError = 0,
        BadValue,
        BadState,
        Unrecovable,
        NotImplemented,
        Unknown,
    };

    TError(EError e = NoError, std::string description = "");
    TError(EError e, int err);

    // return true if non-successful
    operator bool() const;

    int GetError() const;
    const std::string &GetMsg() const;

private:
    EError error;
    std::string description;
};

extern TError NoError;

#endif
