#include <iostream>
#include <csignal>

#include "config.hpp"
#include "version.hpp"
#include "signal.hpp"
#include "unix.hpp"
#include "string.hpp"
#include "test.hpp"
#include "netlink.hpp"

using std::string;

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
}

static int Selftest(int argc, char *argv[]) {
    std::vector<std::string> args;

    for (int i = 0; i < argc; i++)
        args.push_back(argv[i]);

    return test::SelfTest(args);
}

static int Stresstest(int argc, char *argv[]) {
    int threads = -1, iter = 50;
    bool killPorto = true;
    if (argc >= 1)
        StringToInt(argv[0], threads);
    if (argc >= 2)
        StringToInt(argv[1], iter);
    if (argc >= 3 && strcmp(argv[2], "off") == 0)
        killPorto = false;
    std::cout << "Threads: " << threads << " Iterations: " << iter << " Kill: " << killPorto << std::endl;
    return test::StressTest(threads, iter, killPorto);
}

static void Usage() {
    std::cout << "usage: " << program_invocation_short_name << " [--except] <selftest>..." << std::endl;
    std::cout << "       " << program_invocation_short_name << " stress [threads] [iterations] [kill=on/off]" << std::endl;
}

static int TestConnectivity() {
    using namespace test;

    Porto::Connection api;

    std::vector<std::string> containers;
    ExpectApiSuccess(api.List(containers));

    std::string name = "a";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    return 0;
}


int main(int argc, char *argv[])
{
    if (argc == 2 && !strcmp(argv[1], "connectivity"))
        return TestConnectivity();

    // in case client closes pipe we are writing to in the protobuf code
    Signal(SIGPIPE, SIG_IGN);

    umask(0);

    if (argc >= 2) {
        string name(argv[1]);
        if (name == "-h" || name == "--help") {
            Usage();
            return EXIT_FAILURE;
        }

        if (name == "-v" || name == "--version") {
            std::cout << PORTO_VERSION << " " << PORTO_REVISION << std::endl;
            return EXIT_FAILURE;
        }
    }

    ReadConfigs();

    test::InitUsersAndGroups();
    test::InitKernelFeatures();

    std::vector<std::string> v;
    ExpectOk(test::Popen("./portod restart", v));

    string what = "";
    if (argc >= 2)
        what = argv[1];

    if (what == "stress")
        return Stresstest(argc - 2, argv + 2);

    return Selftest(argc - 1, argv + 1);
}
