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

void TVmStat::Dump(rpc::TVmStat &s) {
    #define DUMP_STAT_FIELD(f) s.set_##f(Stat[#f])
    DUMP_STAT_FIELD(count);
    DUMP_STAT_FIELD(size);
    DUMP_STAT_FIELD(max_size);
    DUMP_STAT_FIELD(used);
    DUMP_STAT_FIELD(max_used);
    DUMP_STAT_FIELD(anon);
    DUMP_STAT_FIELD(file);
    DUMP_STAT_FIELD(shmem);
    DUMP_STAT_FIELD(huge);
    DUMP_STAT_FIELD(swap);
    DUMP_STAT_FIELD(data);
    DUMP_STAT_FIELD(stack);
    DUMP_STAT_FIELD(code);
    DUMP_STAT_FIELD(locked);
    DUMP_STAT_FIELD(table);
    #undef DUMP_STAT_FIELD
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

TError GetNetStat(pid_t pid, TUintMap &stats) {
    std::string text, line;
    TError error;

    error = TPath(fmt::format("/proc/{}/net/netstat", pid)).ReadAll(text, 64 << 10);
    if (error)
        return error;

    std::stringstream ss(text);
    std::vector<std::string> headerList;
    std::vector<std::string> valuesList;
    for (int i = 0; std::getline(ss, line); ++i) {
        auto lineList = SplitString(line, ' ');
        if (i % 2 == 0)
            headerList.insert(headerList.end(), lineList.begin(), lineList.end());
        else
            valuesList.insert(valuesList.end(), lineList.begin(), lineList.end());
    }

    if (headerList.size() != valuesList.size())
        return TError("Invalid netstat structure: /proc/{}/net/netstat, {} headers != {} values", pid, headerList.size(), valuesList.size());

    for (int i = 0; i < headerList.size(); ++i) {
        uint64_t value;
        error = StringToUint64(valuesList[i], value);
        if (error)
            continue;
        stats[headerList[i]] = value;
    }
    return OK;
}

TError GetProc(pid_t pid, const std::string &knob, std::string &value) {
    return TPath(fmt::format("/proc/{}/{}", pid, knob)).ReadAll(value, 64 << 10);
}
