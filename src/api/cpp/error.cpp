#include "error.hpp"

extern "C" {
#include <unistd.h>
}

TError::TError() : Error(EError::Success) {
}

TError::TError(EError e, const std::string &description, int eno) :
    Error(e), Description(description), Errno(eno) {
}

TError::TError(EError e, int eno, const std::string &description) :
    Error(e), Description(std::string(strerror(eno)) + ": " + description), Errno(eno) {
}

TError::TError(EError e, std::string &&description, int eno) :
    Error(e), Description(std::move(description)), Errno(eno) {
}

TError::TError(const TError &other, const std::string &prefix) :
    Error(other.Error),
    Description(prefix + ": " + other.Description),
    Errno(other.Errno) {
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
        return FromErrno("Can't serialize error");
    ret = write(fd, &Errno, sizeof(Errno));
    if (ret != sizeof(Errno))
        return FromErrno("Can't serialize errno");
    ret = write(fd, &len, sizeof(len));
    if (ret != sizeof(len))
        return FromErrno("Can't serialize length");
    ret = write(fd, Description.data(), len);
    if (ret != len)
        return FromErrno("Can't serialize description");

    return TError::Success();
}

bool TError::Deserialize(int fd, TError &error) {
    EError err;
    int errno;
    int ret;
    int len;

    ret = read(fd, &err, sizeof(err));
    if (ret == 0)
        return false;
    if (ret != sizeof(Error)) {
        error = FromErrno("Can't deserialize error");
        return true;
    }
    ret = read(fd, &errno, sizeof(Errno));
    if (ret != sizeof(Errno)) {
        error = FromErrno("Can't deserialize errno");
        return true;
    }
    ret = read(fd, &len, sizeof(len));
    if (ret != sizeof(len)) {
        error = FromErrno("Can't deserialize length");
        return true;
    }

    if (len < 0 || len > 4096) {
        error = TError(EError::Unknown, "Invalid error description length: " +
                                        std::to_string(len));
        return true;
    }

    std::string desc(len, '\0');
    ret = read(fd, &desc[0], len);
    if (ret != len) {
        error = FromErrno("Can't deserialize description");
        return true;
    }

    error = TError(err, std::move(desc), errno);
    return true;
}

const TError &TError::Success() {
    static const TError e;
    return e;
}

const TError &TError::Queued() {
    static const TError e(EError::Queued, "Queued");
    return e;
}

TError TError::FromErrno(const std::string &description) {
    return TError(EError::Unknown, errno, description);
}

std::ostream &operator<<(std::ostream &os, const TError &err) {
    os << err.GetErrorName() << " (" << err.GetMsg() << ")";
    return os;
}
