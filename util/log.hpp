#pragma once

#include <fstream>
#include <string>
#include <vector>
#include <streambuf>

#include "util/crash.hpp"
#include "util/path.hpp"

#define PORTO_ASSERT(EXPR) \
    do { \
        if (!(EXPR)) { \
            L_ERR() << "Assertion failed: " << # EXPR << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            Crash(); \
        } \
    } while (0)

#define PORTO_RUNTIME_ERROR(MSG) \
    do { \
        L_ERR() << "Runtime error: " << (MSG) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        Crash(); \
    } while (0)

enum ELogLevel {
    LOG_NOTICE = 0,
    LOG_WARN = 1,
    LOG_ERROR = 2
};

class TLogBuf : public std::streambuf {
    int Fd = -1;
    std::vector<char> Data;
public:
    TLogBuf(const size_t size);
    void Open(const TPath &path, const unsigned int mode);
    int GetFd() { return Fd; }
    void SetFd(int fd) { Fd = fd; }
protected:
    int sync() override;
    int_type overflow(int_type ch) override;
};


class TLogger {
public:
    static void OpenLog(bool std, const TPath &path, const unsigned int mode);
    static void CloseLog();
    static void DisableLog();
    static int GetFd();
    static std::basic_ostream<char> &Log(ELogLevel level = LOG_NOTICE);
    static void LogRequest(const std::string &message);
    static void LogResponse(const std::string &message, size_t execTimeMs);
};

static inline std::basic_ostream<char> &L() { return TLogger::Log(); }
static inline std::basic_ostream<char> &L_ERR() { return TLogger::Log(LOG_ERROR); }
static inline std::basic_ostream<char> &L_WRN() { return TLogger::Log(LOG_WARN); }
