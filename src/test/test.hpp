#pragma once

#include <iostream>
#include <string>
#include <atomic>

#include "libporto.hpp"
#include "util/cred.hpp"
#include "util/path.hpp"

extern "C" {
#include <dirent.h>
}

class TNlLink;

namespace test {
    extern __thread int tid;
    extern std::atomic<int> done;
    extern std::vector<std::shared_ptr<TNlLink>> links;

    std::basic_ostream<char> &Say(std::basic_ostream<char> &stream = std::cout);
    void ExpectReturn(int ret, int exp, const char *func);
    void ExpectError(const TError &ret, const TError &exp, const char *where);
    void ExpectApi(Porto::Connection &api, int ret, int exp, const char *where);

    int ReadPid(const std::string &path);
    int Pgrep(const std::string &name);
    std::string GetRlimit(const std::string &pid, const std::string &type, const bool soft);
    void WaitProcessExit(const std::string &pid, int sec = 10);
    void WaitState(Porto::Connection &api, const std::string &name, const std::string &state, int sec = 10);
    void WaitContainer(Porto::Connection &api, const std::string &name, int sec = 10);
    void WaitPortod(Porto::Connection &api, int times = 10);
    std::string GetCwd(const std::string &pid);
    std::string GetRoot(const std::string &pid);
    std::string GetNamespace(const std::string &pid, const std::string &ns);
    std::map<std::string, std::string> GetCgroups(const std::string &pid);
    std::string GetStatusLine(const std::string &pid, const std::string &prefix);
    std::string GetState(const std::string &pid);
    uint64_t GetCap(const std::string &pid, const std::string &type);
    void GetUidGid(const std::string &pid, int &uid, int &gid);
    std::string GetEnv(const std::string &pid);
    bool CgExists(const std::string &subsystem, const std::string &name);
    std::string CgRoot(const std::string &subsystem, const std::string &name);
    std::string GetFreezer(const std::string &name);
    void SetFreezer(const std::string &name, const std::string &state);
    std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob);
    bool HaveCgKnob(const std::string &subsys, const std::string &knob);
    int GetVmRss(const std::string &pid);
    bool TcClassExist(uint32_t handle);
    bool TcQdiscExist(uint32_t handle);
    bool TcCgFilterExist(uint32_t parent, uint32_t handle);
    int WordCount(const std::string &path, const std::string &word);
    std::string ReadLink(const std::string &path);

    void InitUsersAndGroups();

    extern TCred Nobody;
    extern TCred Alice;
    extern TCred Bob;

    void AsRoot(Porto::Connection &api);
    void AsNobody(Porto::Connection &api);
    void AsAlice(Porto::Connection &api);
    void AsBob(Porto::Connection &api);

    void BootstrapCommand(const std::string &cmd, const TPath &path, bool remove = true);

    void TruncateLogs(Porto::Connection &api);
    void PrintFds(const std::string &path, struct dirent **lst, int nr);
    bool NetworkEnabled();
    void TestDaemon(Porto::Connection &api);

    int SelfTest(std::vector<std::string> args);
    int StressTest(int threads, int iter, bool killPorto);
    int FuzzyTest(int threads, int iter);

    enum class KernelFeature {
        SMART,
        LOW_LIMIT,
        RECHARGE_ON_PGFAULT,
        FSIO,
        CFS_BANDWIDTH,
        CFS_GROUPSCHED,
        IPVLAN,
        MAX_RSS,
        CFQ,
        LAST
    };

    void InitKernelFeatures();
    bool KernelSupports(const KernelFeature &feature);

    void _ExpectEq(size_t ret, size_t exp, const char *where);
    void _ExpectEq(const std::string &ret, const std::string &exp,
                   const char *where);
    void _ExpectNeq(size_t ret, size_t exp, const char *where);
    void _ExpectNeq(const std::string &ret, const std::string &exp,
                    const char *where);
    void _ExpectLess(size_t ret, size_t exp, const char *where);
    void _ExpectLess(const std::string &ret, const std::string &exp,
                     const char *where);
    void _ExpectLessEq(size_t ret, size_t exp, const char *where);
    void _ExpectLessEq(const std::string &ret, const std::string &exp,
                       const char *where);
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define WHERE __FILE__ ":" TOSTRING(__LINE__)

#define Expect(ret) ExpectReturn(ret, true, WHERE)

#define ExpectSuccess(ret) ExpectError(ret, TError::Success(), WHERE)
#define ExpectFailure(ret, exp) ExpectError(ret, exp, WHERE)

#define ExpectApiSuccess(ret) ExpectApi(api, ret, 0, WHERE)
#define ExpectApiFailure(ret, exp) ExpectApi(api, ret, exp, WHERE)

#define ExpectEq(ret, exp) _ExpectEq(ret, exp, WHERE)
#define ExpectNeq(ret, exp) _ExpectNeq(ret, exp, WHERE)
#define ExpectLess(ret, exp) _ExpectLess(ret, exp, WHERE)
#define ExpectLessEq(ret, exp) _ExpectLessEq(ret, exp, WHERE)
