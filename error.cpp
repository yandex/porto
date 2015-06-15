#include "error.hpp"

extern "C" {
#include <string.h>
#include <unistd.h>
}

TError::TError() : Error(EError::Success) {
}

TError::TError(EError e, std::string description, int eno) :
    Error(e), Description(description), Errno(eno) {
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

TError TError::Serialize(int fd) const {
    int ret;
    int len = Description.length();

    ret = write(fd, &Error, sizeof(Error));
    if (ret != sizeof(Error))
        return TError(EError::Unknown, errno, "Can't serialize error");
    ret = write(fd, &Errno, sizeof(Errno));
    if (ret != sizeof(Errno))
        return TError(EError::Unknown, errno, "Can't serialize errno");
    ret = write(fd, &len, sizeof(len));
    if (ret != sizeof(len))
        return TError(EError::Unknown, errno, "Can't serialize length");
    ret = write(fd, Description.data(), len);
    if (ret != len)
        return TError(EError::Unknown, errno, "Can't serialize description");

    return TError::Success();
}

bool TError::Deserialize(int fd, TError &error) {
    EError err;
    int errno;
    std::string desc;
    int ret;
    int len;

    ret = read(fd, &err, sizeof(err));
    if (ret == 0)
        return false;
    if (ret != sizeof(Error)) {
        error = TError(EError::Unknown, errno, "Can't deserialize error");
        return true;
    }
    ret = read(fd, &errno, sizeof(Errno));
    if (ret != sizeof(Errno)) {
        error = TError(EError::Unknown, errno, "Can't deserialize errno");
        return true;
    }
    ret = read(fd, &len, sizeof(len));
    if (ret != sizeof(len)) {
        error = TError(EError::Unknown, errno, "Can't deserialize length");
        return true;
    }

    char *buf = new char[len];
    ret = read(fd, buf, len);
    if (ret != len) {
        delete[] buf;
        error = TError(EError::Unknown, errno, "Can't deserialize description");
        return true;
    }

    desc = std::string(buf, len);
    delete[] buf;

    error = TError(err, desc, errno);
    return true;
}

const TError& TError::Success() {
    static TError e;
    return e;
}

const TError& TError::Queued() {
    static TError e(EError::Queued, "Queued");
    return e;
}

std::ostream& operator<<(std::ostream& os, const TError& err) {
    os << err.GetErrorName() << " (" << err.GetMsg() << ")";
    return os;
}
