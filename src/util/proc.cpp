#include <sstream>

#include "proc.hpp"
#include "path.hpp"

const static TStringMap VmStatMap = {
    {"VmSize", "size"},
    {"VmPeak", "max_size"},
    {"VmRSS", "used"},
    {"VmHWM", "max_used"},
    {"RssAnon", "anon"},
    {"RssFile", "file"},
    {"RssShmem", "shmem"},
    {"HugetlbPages", "huge"},
    {"VmSwap", "swap"},
    {"VmData", "data"},
    {"VmStk", "stack"},
    {"VmExe", "code"},
    {"VmLib", "code"},
    {"VmLck", "locked"},
    {"VmPTE", "table"},
    {"VmPMD", "table"},
};

TVmStat::TVmStat() {
    Reset();
}

void TVmStat::Reset() {
    for (auto &it: VmStatMap)
        Stat[it.second] = 0;
}

void TVmStat::Add(const TVmStat &other) {
    for (auto &it: other.Stat)
        Stat[it.first] += it.second;
}

TError TVmStat::Parse(pid_t pid) {
    std::string text, line;
    TError error;

    error = TPath(fmt::format("/proc/{}/status", pid)).ReadAll(text, 64 << 10);
    if (error)
        return error;

    std::stringstream ss(text);
    while (std::getline(ss, line)) {
        if (!StringEndsWith(line, "kB"))
            continue;

        uint64_t val;
        auto sep = line.find(':');
        if (StringToUint64(line.substr(sep + 1, line.size() - sep - 3), val))
            continue;
        auto key = line.substr(0, sep);

        auto it = VmStatMap.find(key);
        if (it != VmStatMap.end())
            Stat[it->second] += val << 10;
    }
    Stat["count"] += 1;

    return OK;
}

TError GetFdSize(pid_t pid, uint64_t &fdSize) {
    std::string text, line;
    TError error;

    error = TPath(fmt::format("/proc/{}/status", pid)).ReadAll(text, 64 << 10);
    if (error)
        return error;

    std::stringstream ss(text);
    while (std::getline(ss, line)) {
        if (!StringStartsWith(line, "FDSize:"))
            continue;

        static constexpr auto sep = sizeof("FDSize:") - 1;
        error = StringToUint64(line.substr(sep + 1, line.size() - sep), fdSize);
        return error;
    }
    return TError("Cannot find FDSize in /proc/{}/status", pid);
}
