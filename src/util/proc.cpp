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
    s.set_count(Stat["count"]);
    s.set_size(Stat["size"]);
    s.set_max_size(Stat["max_size"]);
    s.set_used(Stat["used"]);
    s.set_max_used(Stat["max_used"]);
    s.set_anon(Stat["anon"]);
    s.set_file(Stat["file"]);
    s.set_shmem(Stat["shmem"]);
    s.set_huge(Stat["huge"]);
    s.set_swap(Stat["swap"]);
    s.set_data(Stat["data"]);
    s.set_stack(Stat["stack"]);
    s.set_code(Stat["code"]);
    s.set_locked(Stat["locked"]);
    s.set_table(Stat["table"]);
}

TError TVmStat::Parse(pid_t pid) {
    TString text, line;
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
