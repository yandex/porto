#pragma once

#include <fstream>
#include <string>
#include <vector>
#include <streambuf>

#include "util/crash.hpp"
#include "util/path.hpp"

enum ELogLevel {
    LOG_NOTICE = 0,
    LOG_WARN = 1,
    LOG_ERROR = 2,
    LOG_EVENT = 3,
    LOG_ACTION = 4,
    LOG_REQUEST = 5,
    LOG_RESPONSE = 6,
    LOG_SYSTEM = 7,
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
};

static inline std::basic_ostream<char> &L() { return TLogger::Log(LOG_NOTICE); }
static inline std::basic_ostream<char> &L_WRN() { return TLogger::Log(LOG_WARN); }
static inline std::basic_ostream<char> &L_ERR() { return TLogger::Log(LOG_ERROR); }
static inline std::basic_ostream<char> &L_EVT() { return TLogger::Log(LOG_EVENT); }
static inline std::basic_ostream<char> &L_ACT() { return TLogger::Log(LOG_ACTION); }
static inline std::basic_ostream<char> &L_REQ() { return TLogger::Log(LOG_REQUEST); }
static inline std::basic_ostream<char> &L_RSP() { return TLogger::Log(LOG_RESPONSE); }
static inline std::basic_ostream<char> &L_SYS() { return TLogger::Log(LOG_SYSTEM); }

std::string RequestAsString(const rpc::TContainerRequest &req);
std::string ResponseAsString(const rpc::TContainerResponse &resp);
