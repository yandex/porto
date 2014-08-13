#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <iostream>
#include <string>

#include "error.hpp"

extern bool verbose;

class TLogger {
public:
    static void OpenLog(const std::string &path);
    static void CloseLog();
    static void Log(const std::string &action);
    static void LogAction(const std::string &action, bool error = false, int errcode = 0);
    static void LogError(const TError &e, const std::string &s);
    static void LogRequest(const std::string &message);
    static void LogResponse(const std::string &message);
};

#endif /* __LOG_HPP__ */
