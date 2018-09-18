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

    int ReadPid(const TString &path);
    int Pgrep(const TString &name);
    TString GetRlimit(const TString &pid, const TString &type, const bool soft);
    void WaitProcessExit(const TString &pid, int sec = 10);
    void WaitState(Porto::Connection &api, const TString &name, const TString &state, int sec = 10);
    void WaitContainer(Porto::Connection &api, const TString &name, int sec = 10);
    void WaitPortod(Porto::Connection &api, int times = 10);
    TString GetCwd(const TString &pid);
    TString GetRoot(const TString &pid);
    TString GetNamespace(const TString &pid, const TString &ns);
    std::map<TString, TString> GetCgroups(const TString &pid);
    TString GetStatusLine(const TString &pid, const TString &prefix);
    TString GetState(const TString &pid);
    uint64_t GetCap(const TString &pid, const TString &type);
    void GetUidGid(const TString &pid, int &uid, int &gid);
    TString GetEnv(const TString &pid);
    bool CgExists(const TString &subsystem, const TString &name);
    TString CgRoot(const TString &subsystem, const TString &name);
    TString GetFreezer(const TString &name);
    void SetFreezer(const TString &name, const TString &state);
    TString GetCgKnob(const TString &subsys, const TString &name, const TString &knob);
    bool HaveCgKnob(const TString &subsys, const TString &knob);
    int GetVmRss(const TString &pid);
    bool TcClassExist(uint32_t handle);
    int WordCount(const TString &path, const TString &word);
    TString ReadLink(const TString &path);

    void InitUsersAndGroups();

    extern TCred Nobody;
    extern TCred Alice;
    extern TCred Bob;

    void AsRoot(Porto::Connection &api);
    void AsNobody(Porto::Connection &api);
    void AsAlice(Porto::Connection &api);
    void AsBob(Porto::Connection &api);

    void BootstrapCommand(const TString &cmd, const TPath &path, bool remove = true);

    void PrintFds(const TString &path, struct dirent **lst, int nr);
    void TestDaemon(Porto::Connection &api);

    int SelfTest(std::vector<TString> args);
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

    TError Popen(const TString &cmd, std::vector<TString> &lines);
}

#pragma GCC diagnostic ignored "-Wsign-compare"
#define STRINGIFY(x) #x
#define Fail(arg) do { fmt::print(stderr, "FAIL {} at {}:{}", (arg), __FILE__, __LINE__); std::abort(); } while(0)
#define ExpectOp(a, op, b) do { auto __a = (a); auto __b = (b); if (!(__a op __b)) { fmt::print(stderr, "FAIL {} ({}) {} {} ({}) at {}:{}", STRINGIFY(a), __a, STRINGIFY(op), STRINGIFY(b), __b, __FILE__, __LINE__); std::abort(); } } while(0)
#define ExpectOk(error) do { if (error) Fail(error); } while(0)
#define ExpectApiSuccess(ret) do { if (ret) Fail(api.GetLastError()); } while (0)
#define ExpectApiFailure(ret, exp) do { if ( (ret) != (exp) ) Fail(api.GetLastError()); } while (0)
#define Expect(a) ExpectOp((bool)(a), ==, true)
#define ExpectEq(a, b) ExpectOp(a, ==, b)
#define ExpectNeq(a, b) ExpectOp(a, !=, b)
#define ExpectLessEq(a, b) ExpectOp(a, <=, b)
#define ExpectState(api, name, state) do { TString __state; if (api.GetProperty(name, "state", __state)) Fail(api.GetLastError()); ExpectEq(__state, state); } while (0)
