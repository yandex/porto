#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <fstream>
#include <string>

#include "util/crash.hpp"
#ifdef PORTOD
#include "util/stat.hpp"
#endif

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

class TLogger {
private:
    static void OpenLog();
public:
    static void InitLog(const std::string &path, const unsigned int mode);
    static void LogToStd();
    static void CloseLog();
    static std::basic_ostream<char> &Log(ELogLevel level = LOG_NOTICE);
    static void LogRequest(const std::string &message);
    static void LogResponse(const std::string &message);
};

static inline std::basic_ostream<char> &L() { return TLogger::Log(); }
static inline std::basic_ostream<char> &L_ERR() { return TLogger::Log(LOG_ERROR); }
static inline std::basic_ostream<char> &L_WRN() { return TLogger::Log(LOG_WARN); }

#endif /* __LOG_HPP__ */
