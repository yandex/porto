#pragma once

#include <string>
#include "statistics.hpp"
#include "util/path.hpp"
#include "fmt/format.h"

extern bool StdLog;
extern bool Verbose;
extern bool Debug;
extern TFile LogFile;

void OpenLog(const TPath &path);
void WriteLog(const char *prefix, const std::string &log_msg);
void Stacktrace();

template <typename... Args> inline void L_DBG(const char* fmt, const Args&... args) {
    if (Debug)
        WriteLog("DBG", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_VERBOSE(const char* fmt, const Args&... args) {
    if (Verbose)
        WriteLog("   ", fmt::format(fmt, args...));
}

template <typename... Args> inline void L(const char* fmt, const Args&... args) {
    WriteLog("   ", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_WRN(const char* fmt, const Args&... args) {
    if (Statistics)
        Statistics->Warns++;
    WriteLog("WRN", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_ERR(const char* fmt, const Args&... args) {
    if (Statistics)
        Statistics->Errors++;
    WriteLog("ERR", fmt::format(fmt, args...));
    if (Verbose)
        Stacktrace();
}

template <typename... Args> inline void L_EVT(const char* fmt, const Args&... args) {
    WriteLog("EVT", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_ACT(const char* fmt, const Args&... args) {
    WriteLog("ACT", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_CG(const char* fmt, const Args&... args) {
    WriteLog("CG ", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_REQ(const char* fmt, const Args&... args) {
    WriteLog("REQ", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_RSP(const char* fmt, const Args&... args) {
    WriteLog("RSP", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_SYS(const char* fmt, const Args&... args) {
    WriteLog("SYS", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_STK(const char* fmt, const Args&... args) {
    WriteLog("STK", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_NET(const char* fmt, const Args&... args) {
    WriteLog("NET", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_NET_VERBOSE(const char* fmt, const Args&... args) {
    if (Verbose)
        WriteLog("NET", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_NL(const char* fmt, const Args&... args) {
    WriteLog("NL ", fmt::format(fmt, args...));
}

template <typename... Args> inline void L_CORE(const char* fmt, const Args&... args) {
    WriteLog("CORE", fmt::format(fmt, args...));
}

void porto_assert(const char *msg, const char *file, size_t line);

#define PORTO_ASSERT(EXPR) do { if (!(EXPR)) porto_assert(#EXPR, __FILE__, __LINE__); } while (0)
#define PORTO_LOCKED(mutex) do { if (mutex.try_lock()) porto_assert(#mutex " not locked", __FILE__, __LINE__); } while(0)
