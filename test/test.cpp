#include <sstream>
#include <csignal>
#include <iomanip>
#include <unordered_map>

#include "test.hpp"
#include "config.hpp"
#include "util/string.hpp"
#include "util/netlink.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

using std::string;
using std::vector;

extern "C" {
#include <unistd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#define _FILE_OFFSET_BITS 64
#include <sys/time.h>
#include <sys/resource.h>
}

namespace test {

__thread int tid;
std::atomic<int> done;

std::basic_ostream<char> &Say(std::basic_ostream<char> &stream) {
    if (tid)
        return stream << "[" << tid << "] ";
    else
        return std::cerr << "- ";
}

int ReadPid(const std::string &path) {
    int pid = 0;

    TError error = TPath(path).ReadInt(pid);
    ExpectOk(error);

    return pid;
}

TError Popen(const std::string &cmd, std::vector<std::string> &lines) {
    FILE *f = popen(cmd.c_str(), "r");
    if (f == nullptr)
        return TError::System("Can't execute " + cmd);

    char *line = nullptr;
    size_t n = 0;

    while (getline(&line, &n, f) >= 0)
        lines.push_back(line);

    auto ret = pclose(f);
    free(line);

    if (ret)
        return TError("popen(" + cmd + ") failed: " + std::to_string(ret));

    return OK;
}

int Pgrep(const std::string &name) {
    vector<string> lines;
    ExpectOk(Popen("pgrep -x " + name + " || true", lines));
    return lines.size();
}

string GetRlimit(const std::string &pid, const std::string &type, const bool soft) {
    const std::unordered_map<std::string, enum __rlimit_resource> resources = {
        {"nproc", RLIMIT_NPROC},
        {"nofile", RLIMIT_NOFILE},
        {"data", RLIMIT_DATA},
        {"memlock", RLIMIT_MEMLOCK}
    };

    struct rlimit limit;
    ExpectEq(prlimit(stoi(pid), resources.at(type), nullptr, &limit), 0);

    return std::to_string(soft ? limit.rlim_cur : limit.rlim_max);
}

void WaitProcessExit(const std::string &pid, int sec) {
    Say() << "Waiting for " << pid << " to exit" << std::endl;

    int times = sec * 10;
    int pidVal = stoi(pid);

    std::string ret;
    do {
        if (times-- <= 0)
            break;

        usleep(100000);

    } while (kill(pidVal, 0) == 0);

    if (times <= 0)
        Fail("Waited too long for process to exit");
}

void WaitContainer(Porto::Connection &api, const std::string &name, int sec) {
    std::string who;
    ExpectApiSuccess(api.WaitContainers({name}, {}, who, sec));
    ExpectEq(who, name);
}

void WaitState(Porto::Connection &api, const std::string &name, const std::string &state, int sec) {
    Say() << "Waiting for " << name << " to be in state " << state << std::endl;

    int times = sec * 10;

    std::string ret;
    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        (void)api.GetData(name, "state", ret);
    } while (ret != state);

    if (times <= 0)
        Fail("Waited too long for task to change state");
}

void WaitPortod(Porto::Connection &api, int times) {
    Say() << "Waiting for portod startup" << std::endl;

    std::vector<std::string> clist;
    do {
        if (times-- <= 0)
            break;

        usleep(1000000);

    } while (api.List(clist) != 0);

    if (times <= 0)
        Fail("Waited too long for portod startup");
}

std::string ReadLink(const std::string &path) {
    TPath lnk;

    TPath f(path);
    TError error = f.ReadLink(lnk);
    ExpectOk(error);

    return lnk.ToString();
}

std::string GetCwd(const std::string &pid) {
    return ReadLink("/proc/" + pid + "/cwd");
}

std::string GetRoot(const std::string &pid) {
    return ReadLink("/proc/" + pid + "/root");
}

std::string GetNamespace(const std::string &pid, const std::string &ns) {
    return ReadLink("/proc/" + pid + "/ns/" + ns);
}

std::map<std::string, std::string> GetCgroups(const std::string &pid) {
    std::map<std::string, std::string> cgmap;
    std::vector<std::string> lines;
    TError error = TPath("/proc/" + pid + "/cgroup").ReadLines(lines);
    ExpectOk(error);

    for (auto l : lines) {
        auto tokens = SplitString(l, ':', 3);
        if (tokens.size() > 2)
            for (auto s : SplitString(tokens[1], ','))
                cgmap[s] = tokens[2];
    }

    return cgmap;
}

std::string GetStatusLine(const std::string &pid, const std::string &prefix) {
    std::vector<std::string> st;
    if (TPath("/proc/" + pid + "/status").ReadLines(st))
        return "";

    for (auto &s : st)
        if (s.substr(0, prefix.length()) == prefix)
            return s;

    return "";
}

//FIXME ugly and racy
std::string GetState(const std::string &pid) {
    std::stringstream ss(GetStatusLine(pid, "State:"));

    std::string name, state, desc;

    ss>> name;
    ss>> state;
    ss>> desc;

    if (name != "State:") {
        if (kill(stoi(pid), 0))
            return "X";
        Fail("PARSING ERROR");
    }

    return state;
}

uint64_t GetCap(const std::string &pid, const std::string &type) {
    auto status = GetStatusLine(pid, type + ":");

    auto fields = SplitString(status, ':');
    if (fields[0] == type)
        return stoull(fields[1], nullptr, 16);

    Fail("PARSING ERROR");
}

void GetUidGid(const std::string &pid, int &uid, int &gid) {
    std::string name;
    std::string stuid = GetStatusLine(pid, "Uid:");
    std::stringstream ssuid(stuid);

    int euid, suid, fsuid;
    ssuid >> name;
    ssuid >> uid;
    ssuid >> euid;
    ssuid >> suid;
    ssuid >> fsuid;

    if (name != "Uid:" || uid != euid || euid != suid || suid != fsuid)
        Fail("Invalid uid");

    std::string stgid = GetStatusLine(pid, "Gid:");
    std::stringstream ssgid(stgid);

    int egid, sgid, fsgid;
    ssgid >> name;
    ssgid >> gid;
    ssgid >> egid;
    ssgid >> sgid;
    ssgid >> fsgid;

    if (name != "Gid:" || gid != egid || egid != sgid || sgid != fsgid)
        Fail("Invalid gid");
}

std::string GetEnv(const std::string &pid) {
    std::string env;
    TError error = TPath("/proc/" + pid + "/environ").ReadAll(env);
    ExpectOk(error);

    return env;
}

bool CgExists(const std::string &subsystem, const std::string &name) {
    return TPath(CgRoot(subsystem, name)).Exists();
}

std::string CgRoot(const std::string &subsystem, const std::string &name) {
    if (name == "/")
        return "/sys/fs/cgroup/" + subsystem + "/";
    if (subsystem == "freezer")
        return "/sys/fs/cgroup/" + subsystem + "/porto/" + name + "/";
    return "/sys/fs/cgroup/" + subsystem + "/porto%" + name + "/";
}

std::string GetFreezer(const std::string &name) {
    std::string state;
    TError error = TPath(CgRoot("freezer", name) + "freezer.state").ReadAll(state);
    ExpectOk(error);
    return state;
}

void SetFreezer(const std::string &name, const std::string &state) {
    TError error = TPath(CgRoot("freezer", name) + "freezer.state").WriteAll(state);
    ExpectOk(error);

    int retries = 1000000;
    while (retries--)
        if (GetFreezer(name) == state + "\n")
            return;

    Fail("Can't set freezer state");
}

std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob) {
    std::string val;
    if (TPath(CgRoot(subsys, name) + knob).ReadAll(val)) {
        Say() << "No " << subsys << " " << name << " " << knob << std::endl;
        return "(none)";
    }
    return StringTrim(val, "\n");
}

bool HaveCgKnob(const std::string &subsys, const std::string &knob) {
    return TPath(CgRoot(subsys, "") + knob).Exists();
}

int GetVmRss(const std::string &pid) {
    std::stringstream ss(GetStatusLine(pid, "VmRSS:"));

    std::string name, size, unit;

    ss>> name;
    ss>> size;
    ss>> unit;

    if (name != "VmRSS:")
        Fail("PARSING ERROR");

    return std::stoi(size);
}

int WordCount(const std::string &path, const std::string &word) {
    int nr = 0;

    std::vector<std::string> lines;
    TError error = TPath(path).ReadLines(lines, 1 << 30);
    ExpectOk(error);

    for (auto s : lines) {
        if (s.find(word) != std::string::npos)
            nr++;
    }

    return nr;
}

TCred Nobody;
TCred Alice;
TCred Bob;

void InitUsersAndGroups() {
    TError error;

    InitPortoGroups();

    error = Nobody.Init("nobody");
    ExpectOk(error);

    error = Alice.Init("porto-alice");
    ExpectOk(error);

    error = Bob.Init("porto-bob");
    ExpectOk(error);

    ExpectNeq(Alice.GetUid(), Bob.GetUid());
    ExpectNeq(Alice.GetGid(), Bob.GetGid());

    Expect(!Nobody.IsMemberOf(PortoGroup));
    Expect(Alice.IsMemberOf(PortoGroup));
    Expect(Bob.IsMemberOf(PortoGroup));
}

void AsRoot(Porto::Connection &api) {
    api.Close();

    ExpectEq(seteuid(0), 0);
    ExpectEq(setegid(0), 0);
    ExpectEq(setgroups(0, nullptr), 0);
}

void AsUser(Porto::Connection &api, TCred &cred) {
    AsRoot(api);

    ExpectEq(setgroups(cred.Groups.size(), cred.Groups.data()), 0);
    ExpectEq(setregid(0, cred.GetGid()), 0);
    ExpectEq(setreuid(0, cred.GetUid()), 0);
}

void AsNobody(Porto::Connection &api) {
    AsUser(api, Nobody);
}

void AsAlice(Porto::Connection &api) {
    AsUser(api, Alice);
}

void AsBob(Porto::Connection &api) {
    AsUser(api, Bob);
}

void BootstrapCommand(const std::string &cmd, const TPath &path, bool remove) {
    if (remove)
        (void)path.RemoveAll();

    vector<string> lines;
    ExpectOk(Popen("ldd " + cmd, lines));

    for (auto &line : lines) {
        auto tokens = SplitString(line, ' ');
        TError error;

        TPath from;
        string name;
        if (tokens.size() == 2) {
            from = StringTrim(tokens[0]);
            TPath p(tokens[0]);
            name = p.BaseName();
        } else if (tokens.size() == 4) {
            if (tokens[2] == "")
                continue;

            from = StringTrim(tokens[2]);
            name = StringTrim(tokens[0]);
        } else {
            continue;
        }

        TPath dest = path / from.DirName();
        if (!dest.Exists()) {
            error = dest.MkdirAll(0755);
            ExpectOk(error);
        }

        Expect(system(("cp " + from.ToString() + " " + dest.ToString() + "/" + name).c_str()) == 0);
    }
    Expect(system(("cp " + cmd + " " + path.ToString()).c_str()) == 0);
}

static int CountSssFds(const std::vector<std::string> &fdPaths) {
    // when sssd is running getgrnam, it opens multiple unix sockets to read database
    std::string sssPrefix = "/var/lib/sss";
    int sssFds = 0;

    for (auto &path: fdPaths) {
        if (std::string(path).substr(0, sssPrefix.length()) == sssPrefix)
            sssFds++;
    }

    return sssFds;
}

static int CountUniqueCgroupFds(const std::vector<std::string> &fdPaths) {
    std::string cgroupPrefix = "/sys/fs/cgroup";
    std::set<std::string> unique_cgroups;

    for (auto &path: fdPaths) {
        if (path.substr(0, cgroupPrefix.length()) == cgroupPrefix)
            unique_cgroups.insert(path);
    }

    return unique_cgroups.size();
}

void PrintFds(const std::string &path, struct dirent **lst, int nr) {
    for (int i = 0; i < nr; i++) {
        if (lst[i]->d_name == string(".") ||
            lst[i]->d_name == string("..")) {
            Say() << "[" << i << "] " << lst[i]->d_name << std::endl;
        } else {
            Say() << "[" << i << "] " << lst[i]->d_name
                << " -> " << ReadLink(path + "/" + lst[i]->d_name) << std::endl;
        }
    }
}

static size_t ChildrenNum(int pid) {
    vector<string> lines;
    ExpectOk(Popen("pgrep -P " + std::to_string(pid) + " || true", lines));
    return lines.size();
}

void TestDaemon(Porto::Connection &api) {
    struct dirent **lst;
    int pid;

    AsRoot(api);

    api.Close();
    sleep(1);

    Say() << "Make sure portod doesn't have zombies" << std::endl;
    pid = ReadPid(PORTO_PIDFILE);
    ExpectEq(ChildrenNum(pid), 0);

    Say() << "Make sure portod doesn't have invalid FDs" << std::endl;

    std::string path = ("/proc/" + std::to_string(pid) + "/fd");

    /**
     * .
     * ..
     * 0 (stdin)
     * 1 (stdout)
     * 128 (event pipe)
     * 129 (ack pipe)
     * 130 (rpc socket)
     * 2 (stderr)
     * 3 (portod.log)
     * 4 (epoll)
     * 5 (host netlink)
     * 6 (signalfd)
     * /run/portod.socket (FIXME: ???)
     * portod private socket (FIXME: ???)
     */
    int appFds = 14 + 2;

    // ctest leaks log fd
    int ctestFd = 1;

    int nr = scandir(path.c_str(), &lst, NULL, alphasort);
    std::vector<std::string> fdPaths;
    for (int i = 0; i < nr; i++) {
        if (lst[i]->d_name == string(".") ||
            lst[i]->d_name == string(".."))
            continue;

        fdPaths.push_back(ReadLink(path + "/" + lst[i]->d_name));
    }

    int cgroupsFd = CountUniqueCgroupFds(fdPaths);
    Expect(cgroupsFd > 0);

    int sssFds = CountSssFds(fdPaths);

    nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    ExpectLessEq(nr, appFds + sssFds + ctestFd + cgroupsFd);
    ExpectLessEq(appFds, nr);

    Say() << "Make sure portod-master doesn't have zombies" << std::endl;
    pid = ReadPid(PORTO_MASTER_PIDFILE);
    ExpectEq(ChildrenNum(pid), 1);

    Say() << "Make sure portod-master doesn't have invalid FDs" << std::endl;
    Say() << "Number of portod-master fds=" << nr << std::endl;
    path = ("/proc/" + std::to_string(pid) + "/fd");
    /**
     * .
     * ..
     * 0 (stdin)
     * 1 (stdout)
     * 130 (rpc socket)
     * 2 (stderr)
     * 3 (portod.log)
     * 4 (epoll)
     * 6 (event pipe)
     * 7 (ack pipe)
     * 9 (signalfd)
     */

    fdPaths.clear();
    appFds = 9 + 2;
    nr = scandir(path.c_str(), &lst, NULL, alphasort);
    for (int i = 0; i < nr; i++) {
        if (lst[i]->d_name == string(".") ||
            lst[i]->d_name == string(".."))
            continue;

        fdPaths.push_back(ReadLink(path + "/" + lst[i]->d_name));
    }

    cgroupsFd = CountUniqueCgroupFds(fdPaths);
    Expect(cgroupsFd == 0);

    sssFds = CountSssFds(fdPaths);

    nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    ExpectLessEq(nr, appFds + sssFds + ctestFd);
    ExpectLessEq(appFds, nr);

    Say() << "Check portod-master queue size" << std::endl;
    std::string v;
    ExpectApiSuccess(api.GetData("/", "porto_stat[queued_statuses]", v));
    Expect(v == std::to_string(0));

    Say() << "Check portod queue size" << std::endl;
    ExpectApiSuccess(api.GetData("/", "porto_stat[queued_events]", v));
    Expect(v != std::to_string(0)); // RotateLogs

    // TODO: check rtnl classes
}

static bool HaveMaxRss() {
    std::vector<std::string> lines;
    TError error = TPath(CgRoot("memory", "") + "memory.stat").ReadLines(lines);
    if (error)
        return false;

    for (auto line : lines) {
        auto tokens = SplitString(line, ' ');
        if (tokens.size() != 2)
            continue;
        if (tokens[0] == "total_max_rss")
            return true;
    }

    return false;
}

static bool IsCfqActive() {
    std::vector<std::string> items;
    (void)TPath("/sys/block").ReadDirectory(items);
    for (auto d : items) {
        if ( (d.find(std::string("loop")) != std::string::npos) || (d.find(std::string("ram")) != std::string::npos) )
            continue;
        std::string data;

        TError error = TPath("/sys/block/" + d + "/queue/scheduler").ReadAll(data);
        ExpectOk(error);
        bool cfqEnabled = false;
        for (auto t : SplitString(data, ' ')) {
            if (t == std::string("[cfq]"))
                cfqEnabled = true;
        }
        if (!cfqEnabled) {
            return false;
        }
    }
    return true;
}

static bool kernel_features[static_cast<int>(KernelFeature::LAST)];

bool KernelSupports(const KernelFeature &feature) {
    return kernel_features[static_cast<int>(feature)];
}

void InitKernelFeatures() {
    kernel_features[static_cast<int>(KernelFeature::CFS_RESERVE)] =
        HaveCgKnob("cpu", "cpu.cfs_reserve_us");
    kernel_features[static_cast<int>(KernelFeature::CFS_BANDWIDTH)] =
        HaveCgKnob("cpu", "cpu.cfs_period_us");
    kernel_features[static_cast<int>(KernelFeature::CFS_GROUPSCHED)] =
        HaveCgKnob("cpu", "cpu.shares");
    kernel_features[static_cast<int>(KernelFeature::FSIO)] =
        HaveCgKnob("memory", "memory.fs_bps_limit");
    kernel_features[static_cast<int>(KernelFeature::LOW_LIMIT)] =
        HaveCgKnob("memory", "memory.low_limit_in_bytes");
    kernel_features[static_cast<int>(KernelFeature::RECHARGE_ON_PGFAULT)] =
        HaveCgKnob("memory", "memory.recharge_on_pgfault");
    kernel_features[static_cast<int>(KernelFeature::MAX_RSS)] = HaveMaxRss();
    kernel_features[static_cast<int>(KernelFeature::CFQ)] = IsCfqActive();

    std::cout << "Kernel features:" << std::endl;
    std::cout << std::left << std::setw(30) << "  CFS_RESERVE" <<
        (KernelSupports(KernelFeature::CFS_RESERVE) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  CFS_BANDWIDTH" <<
        (KernelSupports(KernelFeature::CFS_BANDWIDTH) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  CFS_GROUPSCHED" <<
        (KernelSupports(KernelFeature::CFS_GROUPSCHED) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  FSIO" <<
        (KernelSupports(KernelFeature::FSIO) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  LOW_LIMIT" <<
        (KernelSupports(KernelFeature::LOW_LIMIT) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  RECHARGE_ON_PGFAULT" <<
        (KernelSupports(KernelFeature::RECHARGE_ON_PGFAULT) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  MAX_RSS" <<
        (KernelSupports(KernelFeature::MAX_RSS) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  CFQ" <<
        (KernelSupports(KernelFeature::CFQ) ? "yes" : "no") << std::endl;
}
}
