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
std::vector<std::shared_ptr<TNlLink>> links;

std::basic_ostream<char> &Say(std::basic_ostream<char> &stream) {
    if (tid)
        return stream << "[" << tid << "] ";
    else
        return std::cerr << "- ";
}

void ExpectReturn(int ret, int exp, const char *where) {
    if (ret == exp)
        return;
    throw std::string("Got " + std::to_string(ret) + ", but expected " + std::to_string(exp) + " at " + where);
}

void ExpectError(const TError &ret, const TError &exp, const char *where) {
    std::stringstream ss;

    if (ret == exp)
        return;

    ss << "Got " << ret << ", but expected " << exp << " at " << where;

    throw ss.str();
}

void ExpectApi(Porto::Connection &api, int ret, int exp, const char *where) {
    std::stringstream ss;

    if (ret == exp)
        return;

    int err;
    std::string msg;
    api.GetLastError(err, msg);
    TError error((rpc::EError)err, msg);
    ss << "Got error from libporto: " << error << " (" << ret << " != " << exp << ") at " << where;

    throw ss.str();
}

int ReadPid(const std::string &path) {
    int pid = 0;

    TError error = TPath(path).ReadInt(pid);
    if (error)
        throw std::string(error.GetMsg());

    return pid;
}

int Pgrep(const std::string &name) {
    vector<string> lines;
    ExpectSuccess(Popen("pgrep -x " + name + " || true", lines));
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
        throw std::string("Waited too long for process to exit");
}

void WaitContainer(Porto::Connection &api, const std::string &name, int sec) {
    std::string who;
    ExpectApiSuccess(api.WaitContainers({name}, who, sec));
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
        throw std::string("Waited too long for task to change state");
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
        throw std::string("Waited too long for portod startup");
}

std::string ReadLink(const std::string &path) {
    TPath lnk;

    TPath f(path);
    TError error = f.ReadLink(lnk);
    if (error)
        throw error.GetMsg();

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
    if (error)
        throw std::string("Can't get cgroups: " + error.GetMsg());

    std::vector<std::string> tokens;
    std::vector<std::string> subsys;
    for (auto l : lines) {
        tokens.clear();
        error = SplitString(l, ':', tokens, 3);
        if (error)
            throw std::string("Can't get cgroups: " + error.GetMsg());
        subsys.clear();
        SplitString(tokens[1], ',', subsys);
        for (auto s : subsys)
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
        throw std::string("PARSING ERROR");
    }

    return state;
}

uint64_t GetCap(const std::string &pid, const std::string &type) {
    std::stringstream ss(GetStatusLine(pid, type + ":"));

    std::string name, val;

    ss>> name;
    ss>> val;

    if (name != type + ":")
        throw std::string("PARSING ERROR");

    return stoull(val, nullptr, 16);
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
        throw std::string("Invalid uid");

    std::string stgid = GetStatusLine(pid, "Gid:");
    std::stringstream ssgid(stgid);

    int egid, sgid, fsgid;
    ssgid >> name;
    ssgid >> gid;
    ssgid >> egid;
    ssgid >> sgid;
    ssgid >> fsgid;

    if (name != "Gid:" || gid != egid || egid != sgid || sgid != fsgid)
        throw std::string("Invalid gid");
}

std::string GetEnv(const std::string &pid) {
    std::string env;
    if (TPath("/proc/" + pid + "/environ").ReadAll(env))
        throw std::string("Can't get environment");

    return env;
}

bool CgExists(const std::string &subsystem, const std::string &name) {
    return TPath(CgRoot(subsystem, name)).Exists();
}

std::string CgRoot(const std::string &subsystem, const std::string &name) {
    return "/sys/fs/cgroup/" + subsystem + "/porto/" + name + "/";
}

std::string GetFreezer(const std::string &name) {
    std::string state;
    if (TPath(CgRoot("freezer", name) + "freezer.state").ReadAll(state))
        throw std::string("Can't get freezer");
    return state;
}

void SetFreezer(const std::string &name, const std::string &state) {
    if (TPath(CgRoot("freezer", name) + "freezer.state").WriteAll(state))
        throw std::string("Can't set freezer");

    int retries = 1000000;
    while (retries--)
        if (GetFreezer(name) == state + "\n")
            return;

    throw std::string("Can't set freezer state to ") + state;
}

std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob) {
    std::string val;
    if (TPath(CgRoot(subsys, name) + knob).ReadAll(val))
        throw std::string("Can't get cgroup knob ");
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
        throw std::string("PARSING ERROR");

    return std::stoi(size);
}

bool TcClassExist(uint32_t handle) {
    size_t nr = 0;
    for (auto &link : links) {
        TNlClass tclass(link->GetIndex(), -1, handle);
        if (tclass.Exists(*link->GetNl()))
            nr++;
    }
    return nr == links.size();
}

int WordCount(const std::string &path, const std::string &word) {
    int nr = 0;

    std::vector<std::string> lines;
    if (TPath(path).ReadLines(lines, 1 << 30))
        throw "Can't read log " + path;

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

    error = Nobody.Load("nobody");
    if (error)
        throw error.GetMsg();

    error = Alice.Load("porto-alice");
    if (error)
        throw error.GetMsg();

    error = Bob.Load("porto-bob");
    if (error)
        throw error.GetMsg();

    ExpectNeq(Alice.Uid, Bob.Uid);
    ExpectNeq(Alice.Gid, Bob.Gid);

    Expect(!Nobody.IsPortoUser());
    Expect(Alice.IsPortoUser());
    Expect(Bob.IsPortoUser());
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
    ExpectEq(setregid(0, cred.Gid), 0);
    ExpectEq(setreuid(0, cred.Uid), 0);
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
    ExpectSuccess(Popen("ldd " + cmd, lines));

    for (auto &line : lines) {
        vector<string> tokens;
        TError error = SplitString(line, ' ', tokens);
        if (error)
            throw error.GetMsg();

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
            if (error)
                throw error.GetMsg();
        }

        Expect(system(("cp " + from.ToString() + " " + dest.ToString() + "/" + name).c_str()) == 0);
    }
    Expect(system(("cp " + cmd + " " + path.ToString()).c_str()) == 0);
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

bool NetworkEnabled() {
    return links.size() != 0;
}

static size_t ChildrenNum(int pid) {
    vector<string> lines;
    ExpectSuccess(Popen("pgrep -P " + std::to_string(pid) + " || true", lines));
    return lines.size();
}

void TestDaemon(Porto::Connection &api) {
    struct dirent **lst;
    int pid;

    AsRoot(api);

    api.Close();
    sleep(1);

    Say() << "Make sure portod-slave doesn't have zombies" << std::endl;
    pid = ReadPid(config().slave_pid().path());
    ExpectEq(ChildrenNum(pid), 0);

    Say() << "Make sure portod-slave doesn't have invalid FDs" << std::endl;

    std::string path = ("/proc/" + std::to_string(pid) + "/fd");

    // when sssd is running getgrnam opens unix socket to read database
    int sssFd = 0;
    if (WordCount("/etc/nsswitch.conf", "sss"))
        sssFd = 3;

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
     */
    int nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    ExpectLessEq(nr, 12 + sssFd);
    ExpectLessEq(12, nr);

    Say() << "Make sure portod-master doesn't have zombies" << std::endl;
    pid = ReadPid(config().master_pid().path());
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
     * 3 (portoloop.log)
     * 4 (epoll)
     * 6 (event pipe)
     * 7 (ack pipe)
     * 9 (signalfd)
     */
    nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    ExpectLessEq(nr, 11 + sssFd);
    ExpectLessEq(11, nr);

    Say() << "Check portod-master queue size" << std::endl;
    std::string v;
    ExpectApiSuccess(api.GetData("/", "porto_stat[queued_statuses]", v));
    Expect(v == std::to_string(0));

    Say() << "Check portod-slave queue size" << std::endl;
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
        std::vector<std::string> tokens;
        error = SplitString(line, ' ', tokens);
        if (error)
            return false;

        if (tokens.size() != 2)
            continue;

        if (tokens[0] == "total_max_rss")
            return true;
    }

    return false;
}

static bool HaveIpVlan() {
    auto nl = std::make_shared<TNl>();
    TError error = nl->Connect();
    if (error)
        return false;

    auto link = std::make_shared<TNlLink>(nl, "portoivcheck");
    (void)link->Remove();

    error = link->AddIpVlan(links[0]->GetName(), "l2", -1);
    if (error) {
        return false;
    } else {
        (void)link->Remove();
        return true;
    }
}

static bool IsCfqActive() {
    std::vector<std::string> items;
    (void)TPath("/sys/block").ReadDirectory(items);
    for (auto d : items) {
        if ( (d.find(std::string("loop")) != std::string::npos) || (d.find(std::string("ram")) != std::string::npos) )
            continue;
        std::string data;
        std::vector<std::string> tokens;

        TError error = TPath("/sys/block/" + d + "/queue/scheduler").ReadAll(data);
        if (error)
            throw error.GetMsg();
        error = SplitString(data, ' ', tokens);
        if (error)
            throw error.GetMsg();
        bool cfqEnabled = false;
        for (auto t : tokens) {
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
    kernel_features[static_cast<int>(KernelFeature::SMART)] =
        HaveCgKnob("cpu", "cpu.smart");
    kernel_features[static_cast<int>(KernelFeature::FSIO)] =
        HaveCgKnob("memory", "memory.fs_bps_limit");
    kernel_features[static_cast<int>(KernelFeature::LOW_LIMIT)] =
        HaveCgKnob("memory", "memory.low_limit_in_bytes");
    kernel_features[static_cast<int>(KernelFeature::RECHARGE_ON_PGFAULT)] =
        HaveCgKnob("memory", "memory.recharge_on_pgfault");
    kernel_features[static_cast<int>(KernelFeature::IPVLAN)] = HaveIpVlan();
    kernel_features[static_cast<int>(KernelFeature::MAX_RSS)] = HaveMaxRss();
    kernel_features[static_cast<int>(KernelFeature::CFQ)] = IsCfqActive();

    std::cout << "Kernel features:" << std::endl;
    std::cout << std::left << std::setw(30) << "  SMART" <<
        (KernelSupports(KernelFeature::SMART) ? "yes" : "no") << std::endl;
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
    std::cout << std::left << std::setw(30) << "  IPVLAN" <<
        (KernelSupports(KernelFeature::IPVLAN) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  MAX_RSS" <<
        (KernelSupports(KernelFeature::MAX_RSS) ? "yes" : "no") << std::endl;
    std::cout << std::left << std::setw(30) << "  CFQ" <<
        (KernelSupports(KernelFeature::CFQ) ? "yes" : "no") << std::endl;
}

template<typename T>
static inline void ExpectEqTemplate(T ret, T exp, const char *where) {
    if (ret != exp) {
        Say() << "Unexpected " << ret << " != " << exp << " at " << where << std::endl;
        abort();
    }
}

template<typename T>
static inline void ExpectNeqTemplate(T ret, T exp, const char *where) {
    if (ret == exp) {
        Say() << "Unexpected " << ret << " == " << exp << " at " << where << std::endl;
        abort();
    }
}

template<typename T>
static inline void ExpectLessTemplate(T ret, T exp, const char *where) {
    if (ret >= exp) {
        Say() << "Unexpected " << ret << " >= " << exp << " at " << where << std::endl;
        abort();
    }
}

template<typename T>
static inline void ExpectLessEqTemplate(T ret, T exp, const char *where) {
    if (ret > exp) {
        Say() << "Unexpected " << ret << " > " << exp << " at " << where << std::endl;
        abort();
    }
}

void _ExpectEq(size_t ret, size_t exp, const char *where) {
    ExpectEqTemplate(ret, exp, where);
}

void _ExpectEq(const std::string &ret, const std::string &exp, const char *where) {
    ExpectEqTemplate(ret, exp, where);
}

void _ExpectNeq(size_t ret, size_t exp, const char *where) {
    ExpectNeqTemplate(ret, exp, where);
}

void _ExpectNeq(const std::string &ret, const std::string &exp, const char *where) {
    ExpectNeqTemplate(ret, exp, where);
}

void _ExpectLess(size_t ret, size_t exp, const char *where) {
    ExpectLessTemplate(ret, exp, where);
}

void _ExpectLess(const std::string &ret, const std::string &exp, const char *where) {
    ExpectLessTemplate(ret, exp, where);
}

void _ExpectLessEq(size_t ret, size_t exp, const char *where) {
    ExpectLessEqTemplate(ret, exp, where);
}

void _ExpectLessEq(const std::string &ret, const std::string &exp, const char *where) {
    ExpectLessEqTemplate(ret, exp, where);
}
}
