#pragma once

#include <string>
#include <ostream>
#include "fmt/ostream.h"

#include "rpc.pb.h"

using ::rpc::EError;

class TError {
public:
    EError Error;
    int Errno = 0;
    std::string Text;

    static constexpr unsigned MAX_LENGTH = 65536;

    TError(): Error(EError::Success) {}

    TError(EError err): Error(err) {}

    TError(const std::string &text): Error(EError::Unknown), Text(text) {}

    TError(EError err, const std::string &text): Error(err), Text(text) {}

    TError(EError err, int eno, const std::string &text): Error(err), Errno(eno), Text(text) {}

    template <typename... Args> TError(EError err,  int eno, const char* fmt, const Args&... args):
        Error(err), Errno(eno), Text(fmt::format(fmt, args...)) {}

    template <typename... Args> TError(EError err, const char* fmt, const Args&... args):
        Error(err), Text(fmt::format(fmt, args...)) {}

    template <typename... Args> TError(const char* fmt, const Args&... args):
        Error(EError::Unknown), Text(fmt::format(fmt, args...)) {}

    template <typename... Args> TError(const TError &other, const char* fmt, const Args&... args) :
        Error(other.Error), Errno(other.Errno),
        Text(fmt::format(fmt, args...) + ": " + other.Text) {}

    TError(const TError &other) = default;
    TError(TError &&other) = default;
    TError &operator=(TError &&other) = default;
    TError &operator=(const TError &other) = default;

    explicit operator bool() const {
        return Error != EError::Success;
    }

    inline bool operator==(const EError error) const {
        return Error == error;
    }

    inline bool operator!=(const EError error) const {
        return Error != error;
    }

    static std::string ErrorName(EError error);

    std::string Message() const;
    std::string ToString() const;

    static TError System(const std::string &text) {
        return TError(EError::Unknown, errno, text);
    }

    static TError Queued() {
        return TError(EError::Queued);
    }

    template <typename... Args> static TError System(const char* fmt, const Args&... args) {
        return TError(EError::Unknown, errno, fmt::format(fmt, args...));
    }

    friend std::ostream& operator<<(std::ostream& os, const TError& err);

    TError Serialize(int fd) const;
    static bool Deserialize(int fd, TError &error);

    void Dump(rpc::TError &error) const;
};

extern const TError OK;
