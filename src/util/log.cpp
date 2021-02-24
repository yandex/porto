#include "log.hpp"
#include "util/unix.hpp"
#include "util/signal.hpp"
#include "common.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <execinfo.h>
#include <cxxabi.h>
}

bool StdLog = false;
bool Verbose = false;
bool Debug = false;

__thread char ReqId[9];

TStatistics *Statistics = nullptr;
std::map<std::string, TStatistic> PortoStatMembers;

void InitStatistics() {
    TError error;
    TFile file;

    error = file.Create(PORTOD_STAT_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (!error)
        error = file.Truncate(sizeof(TStatistics));
    if (error) {
        L_ERR("Cannot init {} {}", PORTOD_STAT_FILE, error);
        file.Close();
    }

    Statistics = (TStatistics *)mmap(nullptr, sizeof(TStatistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | (file ? 0 : MAP_ANONYMOUS),
                                     file.Fd, 0);
    PORTO_ASSERT(Statistics != nullptr);

    PortoStatMembers.insert(std::make_pair("spawned", TStatistic(&TStatistics::PortoStarts)));
    PortoStatMembers.insert(std::make_pair("errors", TStatistic(&TStatistics::Errors)));
    PortoStatMembers.insert(std::make_pair("cgerrors", TStatistic(&TStatistics::CgErrors)));
    PortoStatMembers.insert(std::make_pair("warnings", TStatistic(&TStatistics::Warns)));
    PortoStatMembers.insert(std::make_pair("fatals", TStatistic(&TStatistics::Fatals)));
    PortoStatMembers.insert(std::make_pair("taints", TStatistic(&TStatistics::Taints)));
    PortoStatMembers.insert(std::make_pair("postfork_issues", TStatistic(&TStatistics::PostForkIssues)));
    PortoStatMembers.insert(std::make_pair("master_uptime", TStatistic(&TStatistics::MasterStarted, false, true)));
    PortoStatMembers.insert(std::make_pair("porto_uptime", TStatistic(&TStatistics::PortoStarted, false, true)));
    PortoStatMembers.insert(std::make_pair("queued_statuses", TStatistic(&TStatistics::QueuedStatuses, false)));
    PortoStatMembers.insert(std::make_pair("queued_events", TStatistic(&TStatistics::QueuedEvents, false)));
    PortoStatMembers.insert(std::make_pair("remove_dead", TStatistic(&TStatistics::RemoveDead)));
    PortoStatMembers.insert(std::make_pair("restore_failed", TStatistic(&TStatistics::ContainerLost)));
    PortoStatMembers.insert(std::make_pair("start_timeouts", TStatistic(&TStatistics::StartTimeouts)));
    PortoStatMembers.insert(std::make_pair("epoll_sources", TStatistic(&TStatistics::EpollSources, false)));
    PortoStatMembers.insert(std::make_pair("log_lines", TStatistic(&TStatistics::LogLines, false)));
    PortoStatMembers.insert(std::make_pair("log_bytes", TStatistic(&TStatistics::LogBytes, false)));
    PortoStatMembers.insert(std::make_pair("log_lines_lost", TStatistic(&TStatistics::LogLinesLost)));
    PortoStatMembers.insert(std::make_pair("log_bytes_lost", TStatistic(&TStatistics::LogBytesLost)));
    PortoStatMembers.insert(std::make_pair("log_open", TStatistic(&TStatistics::LogOpen)));
    PortoStatMembers.insert(std::make_pair("log_rotate_bytes", TStatistic(&TStatistics::LogRotateBytes)));
    PortoStatMembers.insert(std::make_pair("log_rotate_errors", TStatistic(&TStatistics::LogRotateErrors)));
    PortoStatMembers.insert(std::make_pair("containers", TStatistic(&TStatistics::ContainersCount, false)));
    PortoStatMembers.insert(std::make_pair("containers_created", TStatistic(&TStatistics::ContainersCreated)));
    PortoStatMembers.insert(std::make_pair("containers_started", TStatistic(&TStatistics::ContainersStarted)));
    PortoStatMembers.insert(std::make_pair("containers_failed_start", TStatistic(&TStatistics::ContainersFailedStart)));
    PortoStatMembers.insert(std::make_pair("containers_oom", TStatistic(&TStatistics::ContainersOOM)));
    PortoStatMembers.insert(std::make_pair("containers_tainted", TStatistic(&TStatistics::ContainersTainted)));
    PortoStatMembers.insert(std::make_pair("layer_import", TStatistic(&TStatistics::LayerImport)));
    PortoStatMembers.insert(std::make_pair("layer_export", TStatistic(&TStatistics::LayerExport)));
    PortoStatMembers.insert(std::make_pair("layer_remove", TStatistic(&TStatistics::LayerRemove)));
    PortoStatMembers.insert(std::make_pair("volumes", TStatistic(&TStatistics::VolumesCount, false)));
    PortoStatMembers.insert(std::make_pair("volumes_created", TStatistic(&TStatistics::VolumesCreated)));
    PortoStatMembers.insert(std::make_pair("volumes_failed", TStatistic(&TStatistics::VolumesFailed)));
    PortoStatMembers.insert(std::make_pair("volume_links", TStatistic(&TStatistics::VolumeLinks, false)));
    PortoStatMembers.insert(std::make_pair("volume_links_mounted", TStatistic(&TStatistics::VolumeLinksMounted)));
    PortoStatMembers.insert(std::make_pair("volume_lost", TStatistic(&TStatistics::VolumeLost)));
    PortoStatMembers.insert(std::make_pair("networks", TStatistic(&TStatistics::NetworksCount, false)));
    PortoStatMembers.insert(std::make_pair("networks_created", TStatistic(&TStatistics::NetworksCreated)));
    PortoStatMembers.insert(std::make_pair("network_problems", TStatistic(&TStatistics::NetworkProblems)));
    PortoStatMembers.insert(std::make_pair("network_repairs", TStatistic(&TStatistics::NetworkRepairs)));
    PortoStatMembers.insert(std::make_pair("clients", TStatistic(&TStatistics::ClientsCount, false)));
    PortoStatMembers.insert(std::make_pair("clients_connected", TStatistic(&TStatistics::ClientsConnected)));
    PortoStatMembers.insert(std::make_pair("requests_queued", TStatistic(&TStatistics::RequestsQueued, false)));
    PortoStatMembers.insert(std::make_pair("requests_completed", TStatistic(&TStatistics::RequestsCompleted)));
    PortoStatMembers.insert(std::make_pair("requests_failed", TStatistic(&TStatistics::RequestsFailed)));
    PortoStatMembers.insert(std::make_pair("fail_system", TStatistic(&TStatistics::FailSystem)));
    PortoStatMembers.insert(std::make_pair("fail_invalid_value", TStatistic(&TStatistics::FailInvalidValue)));
    PortoStatMembers.insert(std::make_pair("fail_invalid_command", TStatistic(&TStatistics::FailInvalidCommand)));
    PortoStatMembers.insert(std::make_pair("fail_memory_guarantee", TStatistic(&TStatistics::FailMemoryGuarantee)));
    PortoStatMembers.insert(std::make_pair("fail_invalid_netaddr", TStatistic(&TStatistics::FailInvalidNetaddr)));
    PortoStatMembers.insert(std::make_pair("requests_longer_1s", TStatistic(&TStatistics::RequestsLonger1s)));
    PortoStatMembers.insert(std::make_pair("requests_longer_3s", TStatistic(&TStatistics::RequestsLonger3s)));
    PortoStatMembers.insert(std::make_pair("requests_longer_30s", TStatistic(&TStatistics::RequestsLonger30s)));
    PortoStatMembers.insert(std::make_pair("requests_longer_5m", TStatistic(&TStatistics::RequestsLonger5m)));
    PortoStatMembers.insert(std::make_pair("longest_read_request", TStatistic(&TStatistics::LongestRoRequest)));
    PortoStatMembers.insert(std::make_pair("spec_requests_completed", TStatistic(&TStatistics::SpecRequestsCompleted)));
    PortoStatMembers.insert(std::make_pair("spec_requests_longer_1s", TStatistic(&TStatistics::SpecRequestsLonger1s)));
    PortoStatMembers.insert(std::make_pair("spec_requests_longer_3s", TStatistic(&TStatistics::SpecRequestsLonger3s)));
    PortoStatMembers.insert(std::make_pair("spec_requests_longer_30s", TStatistic(&TStatistics::SpecRequestsLonger30s)));
    PortoStatMembers.insert(std::make_pair("spec_requests_longer_5m", TStatistic(&TStatistics::SpecRequestsLonger5m)));
    PortoStatMembers.insert(std::make_pair("spec_requests_failed", TStatistic(&TStatistics::SpecRequestsFailed)));
    PortoStatMembers.insert(std::make_pair("spec_fail_invalid_value", TStatistic(&TStatistics::SpecRequestsFailedInvalidValue)));
    PortoStatMembers.insert(std::make_pair("spec_fail_unknown", TStatistic(&TStatistics::SpecRequestsFailedUnknown)));
    PortoStatMembers.insert(std::make_pair("spec_fail_no_container", TStatistic(&TStatistics::SpecRequestsFailedContainerDoesNotExist)));
    PortoStatMembers.insert(std::make_pair("lock_operations_count", TStatistic(&TStatistics::LockOperationsCount)));
    PortoStatMembers.insert(std::make_pair("lock_operations_longer_1s", TStatistic(&TStatistics::LockOperationsLonger1s)));
    PortoStatMembers.insert(std::make_pair("lock_operations_longer_3s", TStatistic(&TStatistics::LockOperationsLonger3s)));
    PortoStatMembers.insert(std::make_pair("lock_operations_longer_30s", TStatistic(&TStatistics::LockOperationsLonger30s)));
    PortoStatMembers.insert(std::make_pair("lock_operations_longer_5m", TStatistic(&TStatistics::LockOperationsLonger5m)));
}

TFile LogFile;

void OpenLog() {
    LogFile.SetFd = STDOUT_FILENO;
}

void OpenLog(const TPath &path) {
    int fd;

    if (Statistics)
        Statistics->LogOpen++;

    if (StdLog) {
        fd = STDOUT_FILENO;
    } else {
        struct stat st;
        fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC |
                                O_NOFOLLOW | O_NOCTTY, 0644);
        if (fd >= 0 && !fstat(fd, &st) && (st.st_mode & 0777) != 0644)
            fchmod(fd, 0644);
    }

    if (fd >= 0 && fd < 3)
        fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);

    if (fd >= 0) {
        if (LogFile.Fd != STDOUT_FILENO)
            LogFile.Close();
        LogFile.SetFd = fd;
    }

    /* redirect stdout and stderr into log */
    if (fd >= 0 && !StdLog) {
        dup3(fd, STDOUT_FILENO, O_CLOEXEC);
        dup3(fd, STDERR_FILENO, O_CLOEXEC);
    }
}

void WriteLog(const char *prefix, const std::string &log_msg) {
    std::string reqIdMsg = strlen(ReqId) ? fmt::format("[{}]", ReqId) : "";

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    std::string currentTimeMs = fmt::format("{}.{}", FormatTime(ts.tv_sec), ts.tv_nsec / 1000000);

    std::string msg = fmt::format("{} {}[{}]{}: {} {}\n",
            currentTimeMs, GetTaskName(), GetTid(), reqIdMsg, prefix, log_msg);

    if (Statistics) {
        Statistics->LogLines++;
        Statistics->LogBytes += msg.size();
    }

    if (!LogFile)
        return;

    TError error = LogFile.WriteAll(msg);
    if (error && Statistics) {
        if (error.Errno != ENOSPC &&
                error.Errno != EDQUOT &&
                error.Errno != EROFS &&
                error.Errno != EIO &&
                error.Errno != EUCLEAN)
            Statistics->Warns++;
        Statistics->LogLinesLost++;
        Statistics->LogBytesLost += msg.size();
    }
}

void porto_assert(const char *msg, const char *file, size_t line) {
    L_ERR("Assertion failed: {} at {}:{}", msg, file, line);
    Crash();
}

void FatalError(const std::string &text, TError &error) {
    L_ERR("{}: {}", text, error);
    _exit(EXIT_FAILURE);
}

// https://panthema.net/2008/0901-stacktrace-demangled/
// stacktrace.h (c) 2008, Timo Bingmann from http://idlebox.net/
// published under the WTFPL v2.0

void Stacktrace() {
    L_STK("Stacktrace:");

    // storage array for stack trace address data
    void* addrlist[64];

    // retrieve current stack addresses
    int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
        L_STK("  <empty, possibly corrupt>\n");
        return;
    }

    // resolve addresses into strings containing "filename(function+address)",
    // this array must be free()-ed
    char** symbollist = backtrace_symbols(addrlist, addrlen);

    // allocate string which will be filled with the demangled function name
    size_t funcnamesize = 256;
    char* funcname = (char*)malloc(funcnamesize);

    // iterate over the returned symbol lines. skip the first, it is the
    // address of this function.
    for (int i = 1; i < addrlen; i++) {
        char *begin_name = 0, *begin_offset = 0, *end_offset = 0;
        char *begin_addr = 0;

        // find parentheses and +address offset surrounding the mangled name:
        // ./module(function+0x15c) [0x8048a6d]
        for (char *p = symbollist[i]; *p; ++p) {
            if (*p == '(')
                begin_name = p;
            else if (*p == '+')
                begin_offset = p;
            else if (*p == ')' && begin_offset)
                end_offset = p;
            else if (*p == '[') {
                begin_addr = p;
                break;
            }
        }

        if (begin_name && begin_offset && end_offset && begin_name < begin_offset) {
            *begin_name++ = '\0';
            *begin_offset++ = '\0';
            *end_offset = '\0';

            // mangled name is now in [begin_name, begin_offset) and caller
            // offset in [begin_offset, end_offset). now apply
            // __cxa_demangle():

            int status;
            char* ret = abi::__cxa_demangle(begin_name, funcname, &funcnamesize, &status);
            if (status == 0) {
                funcname = ret; // use possibly realloc()-ed string
                L_STK("{}: {} {}", symbollist[i], funcname, begin_addr);
            } else {
                // demangling failed. Output function name as a C function with no arguments.
                L_STK("{}: {}()+{} {}", symbollist[i], begin_name, begin_offset, begin_addr);
            }
        } else {
            // couldn't parse the line? print the whole line.
            L_STK("{}", symbollist[i]);
        }
    }

    free(funcname);
    free(symbollist);
}

void AccountErrorType(const TError &error) {
    if (Statistics) {
        switch (error.Error) {
            case EError::Unknown:
                Statistics->FailSystem++;
                break;
            case EError::InvalidValue:
                Statistics->FailInvalidValue++;
                break;
            case EError::InvalidCommand:
                Statistics->FailInvalidCommand++;
                break;
            /* InvalidNetworkAddress accounted inside network.cpp */
            default:
                break;
        }
    }
}
