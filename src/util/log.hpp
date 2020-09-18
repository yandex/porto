#pragma once

#include <atomic>
#include <string>
#include "util/path.hpp"
#include "fmt/format.h"

extern bool StdLog;
extern bool Verbose;
extern bool Debug;
extern TFile LogFile;

void OpenLog();
void OpenLog(const TPath &path);
void WriteLog(const char *prefix, const std::string &log_msg);
void Stacktrace();

struct TStatistics {
    std::atomic<uint64_t> PortoStarts;
    std::atomic<uint64_t> Errors;
    std::atomic<uint64_t> Warns;
    std::atomic<uint64_t> Fatals;
    std::atomic<uint64_t> MasterStarted;
    std::atomic<uint64_t> PortoStarted;
    std::atomic<uint64_t> QueuedStatuses;
    std::atomic<uint64_t> QueuedEvents;
    std::atomic<uint64_t> ContainersCreated;
    std::atomic<uint64_t> ContainersStarted;
    std::atomic<uint64_t> ContainersFailedStart;
    std::atomic<uint64_t> ContainersOOM;
    std::atomic<uint64_t> RemoveDead;
    std::atomic<uint64_t> LogLines;
    std::atomic<uint64_t> LogBytes;
    std::atomic<uint64_t> LogRotateBytes;
    std::atomic<uint64_t> LogRotateErrors;
    std::atomic<uint64_t> ContainerLost;
    std::atomic<uint64_t> EpollSources;
    std::atomic<uint64_t> ContainersCount;
    std::atomic<uint64_t> VolumesCount;
    std::atomic<uint64_t> ClientsCount;
    std::atomic<uint64_t> RequestsQueued;
    std::atomic<uint64_t> RequestsCompleted;
    std::atomic<uint64_t> RequestsLonger1s;
    std::atomic<uint64_t> RequestsLonger3s;
    std::atomic<uint64_t> RequestsLonger30s;
    std::atomic<uint64_t> RequestsLonger5m;
    std::atomic<uint64_t> ClientsConnected;
    std::atomic<uint64_t> RequestsFailed;
    std::atomic<uint64_t> SpecRequestsCompleted;
    std::atomic<uint64_t> SpecRequestsLonger1s;
    std::atomic<uint64_t> SpecRequestsLonger3s;
    std::atomic<uint64_t> SpecRequestsLonger30s;
    std::atomic<uint64_t> SpecRequestsLonger5m;
    std::atomic<uint64_t> SpecRequestsFailed;
    std::atomic<uint64_t> SpecRequestsFailedInvalidValue;
    std::atomic<uint64_t> SpecRequestsFailedUnknown;
    std::atomic<uint64_t> SpecRequestsFailedContainerDoesNotExist;
    std::atomic<uint64_t> VolumesCreated;
    std::atomic<uint64_t> VolumesFailed;
    std::atomic<uint64_t> FailSystem;
    std::atomic<uint64_t> FailInvalidValue;
    std::atomic<uint64_t> FailInvalidCommand;
    std::atomic<uint64_t> NetworksCount;
    std::atomic<uint64_t> VolumeLinks;
    std::atomic<uint64_t> VolumeLinksMounted;
    std::atomic<uint64_t> VolumeLost;
    std::atomic<uint64_t> LayerImport;
    std::atomic<uint64_t> LayerExport;
    std::atomic<uint64_t> LayerRemove;
    std::atomic<uint64_t> LogLinesLost;
    std::atomic<uint64_t> LogBytesLost;
    std::atomic<uint64_t> Taints;
    std::atomic<uint64_t> ContainersTainted;
    std::atomic<uint64_t> LongestRoRequest;
    std::atomic<uint64_t> LogOpen;
    std::atomic<uint64_t> FailMemoryGuarantee;
    std::atomic<uint64_t> NetworksCreated;
    std::atomic<uint64_t> NetworkProblems;
    std::atomic<uint64_t> NetworkRepairs;
    std::atomic<uint64_t> CgErrors;
    std::atomic<uint64_t> FailInvalidNetaddr;
    std::atomic<uint64_t> PostForkIssues;

    /* --- add new fields at the end --- */
};

extern TStatistics *Statistics;

void InitStatistics();

static inline void ResetStatistics() {
    Statistics->ContainersCount = 0;
    Statistics->ContainersTainted = 0;
    Statistics->ClientsCount = 0;
    Statistics->VolumesCount = 0;
    Statistics->VolumeLinks = 0;
    Statistics->VolumeLinksMounted = 0;
    Statistics->RequestsQueued = 0;
    Statistics->NetworksCount = 0;
    Statistics->LongestRoRequest = 0;
}

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

template <typename... Args> inline void L_CG_ERR(const char* fmt, const Args&... args) {
    if (Statistics)
        Statistics->CgErrors++;
    WriteLog("CG ERR", fmt::format(fmt, args...));
    if (Verbose)
        Stacktrace();
}

inline void L_TAINT(const std::string &text) {
    if (Statistics)
        Statistics->Taints++;
    WriteLog("TAINT", text);
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
void FatalError(const std::string &text, TError &error);
void AccountErrorType(const TError &error);

#define PORTO_ASSERT(EXPR) do { if (!(EXPR)) porto_assert(#EXPR, __FILE__, __LINE__); } while (0)
#define PORTO_LOCKED(mutex) do { if (mutex.try_lock()) porto_assert(#mutex " not locked", __FILE__, __LINE__); } while(0)
