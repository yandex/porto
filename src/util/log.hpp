#pragma once

#include <fstream>
#include <string>
#include <vector>
#include <streambuf>

#include "util/path.hpp"

#define PORTO_ASSERT(EXPR) do { if (!(EXPR)) porto_assert(#EXPR, __LINE__, __FILE__); } while (0)
#define PORTO_RUNTIME_ERROR(MSG) porto_runtime_error((MSG), __LINE__, __FILE__)
#define PORTO_LOCKED(mutex) do { if (mutex.try_lock()) porto_assert(#mutex " not locked", __LINE__, __FILE__); } while(0)

extern bool Verbose;

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
static inline std::basic_ostream<char> &L_STK() { return TLogger::Log(LOG_STACK); }

void porto_assert(const char *msg, size_t line, const char *file);
void porto_runtime_error(const std::string &msg, size_t line, const char *file);
