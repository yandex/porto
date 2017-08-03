#pragma once

#include <fstream>
#include <string>
#include <vector>
#include <streambuf>

#include "util/path.hpp"
#include "fmt/format.h"

#define PORTO_ASSERT(EXPR) do { if (!(EXPR)) porto_assert(#EXPR, __LINE__, __FILE__); } while (0)
#define PORTO_RUNTIME_ERROR(MSG) porto_runtime_error((MSG), __LINE__, __FILE__)
#define PORTO_LOCKED(mutex) do { if (mutex.try_lock()) porto_assert(#mutex " not locked", __LINE__, __FILE__); } while(0)

extern bool Verbose;
extern bool Debug;

enum ELogLevel {
    LOG_NOTICE = 0,
    LOG_WARN = 1,
    LOG_ERROR = 2,
    LOG_EVENT = 3,
    LOG_ACTION = 4,
    LOG_REQUEST = 5,
    LOG_RESPONSE = 6,
    LOG_SYSTEM = 7,
    LOG_STACK = 8,
};

class TLogger {
public:
    static void OpenLog(bool std, const TPath &path, const unsigned int mode);
    static void CloseLog();
    static void DisableLog();
    static int GetFd();
    static void Log(std::string log_msg, ELogLevel level = LOG_NOTICE);
};

template <typename... Args> inline void L(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_NOTICE);
}

template <typename... Args> inline void L_WRN(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_WARN);
}

template <typename... Args> inline void L_ERR(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_ERROR);
}

template <typename... Args> inline void L_EVT(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_EVENT);
}

template <typename... Args> inline void L_ACT(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_ACTION);
}

template <typename... Args> inline void L_REQ(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_REQUEST);
}

template <typename... Args> inline void L_RSP(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_RESPONSE);
}

template <typename... Args> inline void L_SYS(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_SYSTEM);
}

template <typename... Args> inline void L_STK(const char* fmt, const Args&... args) {
    TLogger::Log(fmt::format(fmt, args...), LOG_STACK);
}

void porto_assert(const char *msg, size_t line, const char *file);
void porto_runtime_error(const std::string &msg, size_t line, const char *file);
