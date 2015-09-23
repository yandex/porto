#include <iostream>
#include <csignal>

#include "config.hpp"
#include "version.hpp"
#include "qdisc.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/log.hpp"
#include "test/test.hpp"

using std::string;

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
}

static int Selftest(int argc, char *argv[]) {
    std::vector<std::string> test;
    int leakNr = 1000;

    for (int i = 0; i < argc; i++) {
        TError error = StringToInt(argv[i], leakNr);
        if (error)
            test.push_back(argv[i]);
    }

    return test::SelfTest(test, leakNr);
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

static int Fuzzytest(int argc, char *argv[]) {
    int threads = 32, iter = 1000;
    if (argc >= 1)
        StringToInt(argv[0], threads);
    if (argc >= 2)
        StringToInt(argv[1], iter);
    std::cout << "Threads: " << threads << " Iterations: " << iter << std::endl;
    return test::FuzzyTest(threads, iter);
}

static void Usage() {
    std::cout << "usage: " << program_invocation_short_name << " [selftest name]" << std::endl;
    std::cout << "       " << program_invocation_short_name << " stress [threads] [iterations] [kill=on/off]" << std::endl;
}

static int TestConnectivity() {
    using namespace test;
    config.Load();
    TPortoAPI api(config().rpc_sock().file().path(), 0);

    std::vector<std::string> containers;
    ExpectApiSuccess(api.List(containers));

    std::string name = "a";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    return 0;
}

int main(int argc, char *argv[])
{
    // in case client closes pipe we are writing to in the protobuf code
    (void)RegisterSignal(SIGPIPE, SIG_IGN);

    TLogger::DisableLog();

    umask(0);

    if (argc >= 2) {
        string name(argv[1]);
        if (name == "-h" || name == "--help") {
            Usage();
            return EXIT_FAILURE;
        }

        if (name == "-v" || name == "--version") {
            std::cout << GIT_TAG << " " << GIT_REVISION <<std::endl;
            return EXIT_FAILURE;
        }
    }

    try {
        config.Load();

        auto net = std::make_shared<TNetwork>();
        if (config().network().enabled()) {
            auto lock = net->ScopedLock();
            TError error = net->OpenLinks(test::links);
            if (error)
                throw error.GetMsg();
        }

        test::InitKernelFeatures();

        string what = "";
        if (argc >= 2)
            what = argv[1];

        if (what == "stress")
            return Stresstest(argc - 2, argv + 2);
        if (what == "fuzzy")
            return Fuzzytest(argc - 2, argv + 2);
        else if (what == "connectivity")
            return TestConnectivity();
        else
            return Selftest(argc - 1, argv + 1);
    } catch (string err) {
        std::cerr << "Exception: " << err << std::endl;
    } catch (const std::exception &exc) {
        std::cerr << "Exception: " << exc.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown exception" << std::endl;
    }

    return EXIT_FAILURE;
};
