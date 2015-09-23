#include <sstream>
#include <csignal>
#include <iomanip>
#include <unordered_map>

#include "test.hpp"
#include "config.hpp"
#include "error.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/string.hpp"
#include "util/netlink.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

using std::string;
using std::vector;

extern "C" {
#include <unistd.h>
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

void ExpectReturn(int ret, int exp, int line, const char *func) {
    if (ret == exp)
        return;
    throw std::string("Got " + std::to_string(ret) + ", but expected " + std::to_string(exp) + " at " + func + ":" + std::to_string(line));
}

void ExpectError(const TError &ret, const TError &exp, int line, const char *func) {
    std::stringstream ss;

    if (ret == exp)
        return;

    ss << "Got " << ret << ", but expected " << exp << " at " << func << ":" << line;

    throw ss.str();
}

void ExpectApi(TPortoAPI &api, int ret, int exp, int line, const char *func) {
    std::stringstream ss;

    if (ret == exp)
        return;

    int err;
    std::string msg;
    api.GetLastError(err, msg);
    TError error((rpc::EError)err, msg);
    ss << "Got error from libporto: " << error << " (" << ret << " != " << exp << ") at " << func << ":" << line;

    throw ss.str();
}

int ReadPid(const std::string &path) {
    TFile f(path);
    int pid = 0;

    TError error = f.AsInt(pid);
    if (error)
        throw std::string(error.GetMsg());

    return pid;
}

int Pgrep(const std::string &name) {
    vector<string> lines;
    ExpectSuccess(Popen("pgrep -x " + name, lines));
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

void WaitContainer(TPortoAPI &api, const std::string &name, int sec) {
    std::string who;
    ExpectApiSuccess(api.Wait({name}, who, sec * 1000));
    ExpectEq(who, name);
}

void WaitState(TPortoAPI &api, const std::string &name, const std::string &state, int sec) {
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

void WaitPortod(TPortoAPI &api, int times) {
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
    TFile f("/proc/" + pid + "/cgroup");
    std::vector<std::string> lines;
    TError error = f.AsLines(lines);
    if (error)
        throw std::string("Can't get cgroups: " + error.GetMsg());

    std::vector<std::string> tokens;
    for (auto l : lines) {
        tokens.clear();
        error = SplitString(l, ':', tokens, 3);
        if (error)
            throw std::string("Can't get cgroups: " + error.GetMsg());
        cgmap[tokens[1]] = tokens[2];
    }

    return cgmap;
}

std::string GetStatusLine(const std::string &pid, const std::string &prefix) {
    std::vector<std::string> st;
    TFile f("/proc/" + pid + "/status");
    if (f.AsLines(st))
        return "";

    for (auto &s : st)
        if (s.substr(0, prefix.length()) == prefix)
            return s;

    return "";
}

std::string GetState(const std::string &pid) {
    std::stringstream ss(GetStatusLine(pid, "State:"));

    std::string name, state, desc;

    ss>> name;
    ss>> state;
    ss>> desc;

    if (name != "State:")
        throw std::string("PARSING ERROR");

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

int UserUid(const std::string &user) {
    TUser u(user);
    TError error = u.Load();
    if (error)
        throw error.GetMsg();

    return u.GetId();
}

int GroupGid(const std::string &group) {
    TGroup g(group);
    TError error = g.Load();
    if (error)
        throw error.GetMsg();

    return g.GetId();
}

std::string GetEnv(const std::string &pid) {
    std::string env;
    TFile f("/proc/" + pid + "/environ");
    if (f.AsString(env))
        throw std::string("Can't get environment");

    return env;
}

bool CgExists(const std::string &subsystem, const std::string &name) {
    TFile f(CgRoot(subsystem, name));
    return f.Exists();
}

std::string CgRoot(const std::string &subsystem, const std::string &name) {
    return "/sys/fs/cgroup/" + subsystem + "/porto/" + name + "/";
}

std::string GetFreezer(const std::string &name) {
    std::string state;
    TFile m(CgRoot("freezer", name) + "freezer.state");
    if (m.AsString(state))
        throw std::string("Can't get freezer");
    return state;
}

void SetFreezer(const std::string &name, const std::string &state) {
    TFile m(CgRoot("freezer", name) + "freezer.state");
    if (m.WriteStringNoAppend(state))
        throw std::string("Can't set freezer");

    int retries = 1000000;
    while (retries--)
        if (GetFreezer(name) == state + "\n")
            return;

    throw std::string("Can't set freezer state to ") + state;
}

std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob) {
    std::string val;
    TFile m(CgRoot(subsys, name) + knob);
    if (m.AsString(val))
        throw std::string("Can't get cgroup knob " + m.GetPath().ToString());
    return StringTrim(val, "\n");
}

bool HaveCgKnob(const std::string &subsys, const std::string &knob) {
    std::string val;
    TFile m(CgRoot(subsys, "") + knob);
    return m.Exists();
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
        ExpectSuccess(link->RefillClassCache());
        TNlClass tclass(link, -1, handle);
        if (tclass.Exists())
            nr++;
    }
    return nr == links.size();
}

bool TcQdiscExist(uint32_t handle) {
    size_t nr = 0;
    for (auto &link : links) {
        TNlHtb qdisc(link, -1, handle);
        if (qdisc.Exists())
            nr++;
    }
    return nr == links.size();
}

bool TcCgFilterExist(uint32_t parent, uint32_t handle) {
    size_t nr = 0;
    for (auto &link : links) {
        TNlCgFilter filter(link, parent, handle);
        if (filter.Exists())
            nr++;
    }
    return nr == links.size();
}

int WordCount(const std::string &path, const std::string &word) {
    int nr = 0;

    std::vector<std::string> lines;
    TFile log(path);
    if (log.AsLines(lines))
        throw "Can't read log " + path;

    for (auto s : lines) {
        if (s.find(word) != std::string::npos)
            nr++;
    }

    return nr;
}

bool FileExists(const std::string &path) {
    TFile f(path);
    return f.Exists();
}

void AsUser(TPortoAPI &api, TUser &user, TGroup &group) {
    AsRoot(api);

    Expect(setregid(0, group.GetId()) == 0);
    Expect(setreuid(0, user.GetId()) == 0);
}

void AsRoot(TPortoAPI &api) {
    api.Cleanup();

    Expect(seteuid(0) == 0);
    Expect(setegid(0) == 0);
}

void AsNobody(TPortoAPI &api) {
    TUser nobody(GetDefaultUser());
    TError error = nobody.Load();
    if (error)
        throw error.GetMsg();

    TGroup nogroup(GetDefaultGroup());
    error = nogroup.Load();
    if (error)
        throw error.GetMsg();

    AsUser(api, nobody, nogroup);
}

void AsDaemon(TPortoAPI &api) {
    TUser daemonUser("daemon");
    TError error = daemonUser.Load();
    if (error)
        throw error.GetMsg();

    TGroup daemonGroup("daemon");
    error = daemonGroup.Load();
    if (error)
        throw error.GetMsg();

    AsUser(api, daemonUser, daemonGroup);
}

std::string GetDefaultUser() {
    std::string users[] = { "nobody" };

    for (auto &user : users) {
        TUser u(user);
        TError error = u.Load();
        if (!error)
            return u.GetName();
    }

    return "daemon";
}

std::string GetDefaultGroup() {
    std::string groups[] = { "nobody", "nogroup" };

    for (auto &group : groups) {
        TGroup g(group);
        TError error = g.Load();
        if (!error)
            return g.GetName();
    }

    return "daemon";
}

void BootstrapCommand(const std::string &cmd, const std::string &path, bool remove) {
    TFolder d(path);
    if (remove)
        (void)d.Remove(true);

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

        TFolder dest(path + "/" + from.DirName().ToString());
        if (!dest.Exists()) {
            error = dest.Create(0755, true);
            if (error)
                throw error.GetMsg();
        }

        Expect(system(("cp " + from.ToString() + " " + dest.GetPath().ToString() + "/" + name).c_str()) == 0);
    }
    Expect(system(("cp " + cmd + " " + path).c_str()) == 0);
}

void RotateDaemonLogs(TPortoAPI &api) {
    // Truncate slave log
    TFile slaveLog(config().slave_log().path());
    ExpectSuccess(slaveLog.Remove());

    int pid = ReadPid(config().slave_pid().path());
    if (kill(pid, SIGUSR1))
        throw string("Can't send SIGUSR1 to slave");

    WaitPortod(api);

    // Truncate master log
    TFile masterLog(config().master_log().path());
    ExpectSuccess(masterLog.Remove());

    pid = ReadPid(config().master_pid().path());
    if (kill(pid, SIGUSR1))
        throw string("Can't send SIGUSR1 to master");

    WaitPortod(api);
}

void RestartDaemon(TPortoAPI &api) {
    std::cerr << ">>> Truncating logs and restarting porto..." << std::endl;

    if (Pgrep("portod") != 1)
        throw string("Porto is not running (or multiple portod processes)");

    if (Pgrep("portod-slave") != 1)
        throw string("Porto slave is not running");

    // Remove porto cgroup to clear statistics
    int pid = ReadPid(config().master_pid().path());
    if (kill(pid, SIGINT))
        throw string("Can't send SIGINT to slave");

    // We need to wait longer because porto may need to remove huge number of
    // containers
    WaitPortod(api, 5 * 60);

    RotateDaemonLogs(api);

    // Clean statistics
    pid = ReadPid(config().master_pid().path());
    if (kill(pid, SIGHUP))
        throw string("Can't send SIGHUP to master");

    WaitPortod(api);
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
    ExpectSuccess(Popen("pgrep -P " + std::to_string(pid), lines));
    return lines.size();
}

void TestDaemon(TPortoAPI &api) {
    struct dirent **lst;
    int pid;

    AsRoot(api);

    api.Cleanup();
    sleep(1);

    Say() << "Make sure portod-slave doesn't have zombies" << std::endl;
    pid = ReadPid(config().slave_pid().path());
    ExpectEq(ChildrenNum(pid), 0);

    Say() << "Make sure portod-slave doesn't have invalid FDs" << std::endl;

    std::string path = ("/proc/" + std::to_string(pid) + "/fd");

    // when sssd is running getgrnam opens unix socket to read database
    int sssFd = 0;
    if (WordCount("/etc/nsswitch.conf", "sss"))
        sssFd = 2;

    int nl = NetworkEnabled() ? 1 : 0;

    if (config().network().dynamic_ifaces())
        nl++; // event netlink

    // . .. 0(stdin) 1(stdout) 2(stderr) 3(log) 4(rpc socket) 5(epoll) 6(netlink socket) 128(event pipe) 129(ack pipe)
    int nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    Expect(nr >= 2 + 8 + nl && nr <= 2 + 8 + nl + sssFd);

    Say() << "Make sure portod-master doesn't have zombies" << std::endl;
    pid = ReadPid(config().master_pid().path());
    ExpectEq(ChildrenNum(pid), 1);

    Say() << "Make sure portod-master doesn't have invalid FDs" << std::endl;
    Say() << "Number of portod-master fds=" << nr << std::endl;
    path = ("/proc/" + std::to_string(pid) + "/fd");

    // . .. 0(stdin) 1(stdout) 2(stderr) 3(log) 4(epoll) 5(event pipe) 6(ack pipe)
    nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    Expect(nr == 2 + 7);

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
    TFile f(CgRoot("memory", "") + "memory.stat");

    std::vector<std::string> lines;
    TError error = f.AsLines(lines);
    if (error)
        return false;

    for (auto line : lines) {
        std::vector<std::string> tokens;
        error = SplitString(line, ' ', tokens);
        if (error)
            return false;

        if (tokens.size() != 2)
            continue;

        if (tokens[0] == "max_rss")
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

    error = link->AddIpVlan(links[0]->GetAlias(), "l2", -1);
    if (error) {
        return false;
    } else {
        (void)link->Remove();
        return true;
    }
}

static bool IsCfqActive() {
    TFolder f("/sys/block");
    std::vector<std::string> items;
    (void)f.Items(EFileType::Any, items);
    for (auto d : items) {
        if ( (d.find(std::string("loop")) != std::string::npos) || (d.find(std::string("ram")) != std::string::npos) )
            continue;
        TFile f(std::string("/sys/block/" + d + "/queue/scheduler"));
        std::string data;
        std::vector<std::string> tokens;

        TError error = f.AsString(data);
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
static inline void ExpectEqTemplate(T ret, T exp, size_t line, const char *func) {
    if (ret != exp) {
        Say() << "Unexpected " << ret << " != " << exp << " at " << func << ":" << line << std::endl;
        abort();
    }
}

template<typename T>
static inline void ExpectNeqTemplate(T ret, T exp, size_t line, const char *func) {
    if (ret == exp) {
        Say() << "Unexpected " << ret << " == " << exp << " at " << func << ":" << line << std::endl;
        abort();
    }
}

template<typename T>
static inline void ExpectLessTemplate(T ret, T exp, size_t line, const char *func) {
    if (ret >= exp) {
        Say() << "Unexpected " << ret << " >= " << exp << " at " << func << ":" << line << std::endl;
        abort();
    }
}

void _ExpectEq(size_t ret, size_t exp, size_t line, const char *func) {
    ExpectEqTemplate(ret, exp, line, func);
}

void _ExpectEq(const std::string &ret, const std::string &exp, size_t line, const char *func) {
    ExpectEqTemplate(ret, exp, line, func);
}

void _ExpectNeq(size_t ret, size_t exp, size_t line, const char *func) {
    ExpectNeqTemplate(ret, exp, line, func);
}

void _ExpectNeq(const std::string &ret, const std::string &exp, size_t line, const char *func) {
    ExpectNeqTemplate(ret, exp, line, func);
}

void _ExpectLess(size_t ret, size_t exp, size_t line, const char *func) {
    ExpectLessTemplate(ret, exp, line, func);
}

void _ExpectLess(const std::string &ret, const std::string &exp, size_t line, const char *func) {
    ExpectLessTemplate(ret, exp, line, func);
}
}
