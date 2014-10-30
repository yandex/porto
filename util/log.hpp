#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <fstream>
#include <string>

#include "error.hpp"

#define PORTO_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            TLogger::Log() << "Assertion failed: " << # expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            abort(); \
        } \
    } while (0)

class TLogger {
private:
    static void OpenLog();
public:
    static void InitLog(const std::string &path, const unsigned int mode, const bool verbose);
    static void LogToStd();
    static void CloseLog();
    static void TruncateLog();
    static std::basic_ostream<char> &Log();
    static void LogAction(const std::string &action, bool error = false, int errcode = 0);
    static void LogRequest(const std::string &message);
    static void LogResponse(const std::string &message);

    static void LogError(const TError &e, const std::string &s) {
        if (!e)
            return;

        Log() << "Error(" << e.GetErrorName() << "): " << s << ": " << e.GetMsg() << std::endl;
    }
};

#endif /* __LOG_HPP__ */
