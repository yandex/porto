#include "util/error.hpp"

extern "C" {
#include <unistd.h>
}

std::string TError::ErrorName(EError error) {
    return rpc::EError_Name(error);
}

std::string TError::Message() const {
    if (Errno)
        return fmt::format("{}: {}", strerror(Errno), Text);
    return Text;
}

std::string TError::ToString() const {
    if (Errno)
        return fmt::format("{}:({}: {})", ErrorName(Error), strerror(Errno), Text);
    if (Text.length())
        return fmt::format("{}:({})", ErrorName(Error), Text);
    return ErrorName(Error);
}

TError TError::Serialize(int fd) const {
    int ret;
    unsigned len = Text.length();

    ret = write(fd, &Error, sizeof(Error));
    if (ret != sizeof(Error))
        return System("Can't serialize error");
    ret = write(fd, &Errno, sizeof(Errno));
    if (ret != sizeof(Errno))
        return System("Can't serialize errno");
    ret = write(fd, &len, sizeof(len));
    if (ret != sizeof(len))
        return System("Can't serialize length");
    ret = write(fd, Text.data(), len);
    if (ret != (int)len)
        return System("Can't serialize description");

    return OK;
}

bool TError::Deserialize(int fd, TError &error) {
    EError err;
    int errno_;
    int ret;
    unsigned len;

    ret = read(fd, &err, sizeof(err));
    if (ret == 0)
        return false;
    if (ret != sizeof(Error) || !EError_IsValid(err)) {
        error = System("Can't deserialize error");
        return true;
    }
    ret = read(fd, &errno_, sizeof(Errno));
    if (ret != sizeof(Errno)) {
        error = System("Can't deserialize errno");
        return true;
    }
    ret = read(fd, &len, sizeof(len));
    if (ret != sizeof(len)) {
        error = System("Can't deserialize length");
        return true;
    }

    if (len > TError::MAX_LENGTH) {
        error = TError("Invalid error description length: {}", len);
        return true;
    }

    std::string desc(len, '\0');
    ret = read(fd, &desc[0], len);
    if (ret != (int)len) {
        error = System("Can't deserialize description");
        return true;
    }

    error = TError(err, errno_, std::move(desc));
    return true;
}

void TError::Dump(rpc::TError &error) const {
    error.set_error(Error);
    error.set_msg(Message());
}

std::ostream &operator<<(std::ostream &os, const TError &err) {
    os << err.ToString();
    return os;
}

const TError OK;
