#pragma once

#include <iostream>
#include <string>
#include <atomic>

#include "fmt/format.h"

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

    std::basic_ostream<char> &Say(std::basic_ostream<char> &stream = std::cout);

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

    void PrintFds(const std::string &path, struct dirent **lst, int nr);
    void TestDaemon(Porto::Connection &api);

    int SelfTest(std::vector<std::string> args);
    int StressTest(int threads, int iter, bool killPorto);
    int FuzzyTest(int threads, int iter);

    enum class KernelFeature {
        LOW_LIMIT,
        RECHARGE_ON_PGFAULT,
        FSIO,
        CFS_BANDWIDTH,
        CFS_GROUPSCHED,
        CFS_RESERVE,
        MAX_RSS,
        CFQ,
        LAST
    };

    void InitKernelFeatures();
    bool KernelSupports(const KernelFeature &feature);

    TError Popen(const std::string &cmd, std::vector<std::string> &lines);
}

#pragma GCC diagnostic ignored "-Wsign-compare"
#define Fail(arg) do { fmt::print(stderr, "FAIL {} at {}:{}", (arg), __FILE__, __LINE__); std::abort(); } while(0)
#define ExpectOp(a, op, b) do { auto __a = (a); auto __b = (b); if (!(__a op __b)) { fmt::print(stderr, "FAIL {} ({}) {} {} ({}) at {}:{}", STRINGIFY(a), __a, STRINGIFY(op), STRINGIFY(b), __b, __FILE__, __LINE__); std::abort(); } } while(0)
#define ExpectOk(error) do { if (error) Fail(error); } while(0)
#define ExpectApiSuccess(ret) do { if (ret) Fail(api.GetLastError()); } while (0)
#define ExpectApiFailure(ret, exp) do { if ( (ret) != (exp) ) Fail(api.GetLastError()); } while (0)
#define Expect(a) ExpectOp((bool)(a), ==, true)
#define ExpectEq(a, b) ExpectOp(a, ==, b)
#define ExpectNeq(a, b) ExpectOp(a, !=, b)
#define ExpectLessEq(a, b) ExpectOp(a, <=, b)
#define ExpectState(api, name, state) do { std::string __state; if (api.GetData(name, "state", __state)) Fail(api.GetLastError()); ExpectEq(__state, state); } while (0)
