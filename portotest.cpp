#include <iostream>
#include <csignal>

#include "util/unix.hpp"
#include "util/string.hpp"
#include "test/test.hpp"

using namespace std;

static int Selftest(int argc, char *argv[]) {
    string test = "";
    int leakNr = 1000;
    if (argc >= 1) {
        TError error = StringToInt(argv[0], leakNr);
        if (error)
            test = argv[0];
    }
    return Test::SelfTest(test, leakNr);
}

static int Stresstest(int argc, char *argv[]) {
    int threads = 1, iter = 1000;
    bool killPorto = true;
    if (argc >= 1)
        StringToInt(argv[0], threads);
    if (argc >= 2)
        StringToInt(argv[1], iter);
    if (argc >= 3 && strcmp(argv[2], "off") == 0)
        killPorto = false;
    cout << "Threads: " << threads << " Iterations: " << iter << " Kill: " << killPorto << endl;
    return Test::StressTest(threads, iter, killPorto);
}

static void Usage() {
    cout << "usage: " << program_invocation_short_name << " [selftest name]" << endl;
    cout << "       " << program_invocation_short_name << " stress [threads] [iterations] [kill=on/off]" << endl;
}

int main(int argc, char *argv[])
{
    // in case client closes pipe we are writing to in the protobuf code
    (void)RegisterSignal(SIGPIPE, SIG_IGN);

    if (argc >= 2) {
        string name(argv[1]);
        if (name == "-h" || name == "--help") {
            Usage();
            return EXIT_FAILURE;
        }

        if (name == "-v" || name == "--version") {
            cout << GIT_TAG << " " << GIT_REVISION <<endl;
            return EXIT_FAILURE;
        }
    }

    try {
        string what = "";
        if (argc >= 2)
            what = argv[1];

        if (what == "stress")
            return Stresstest(argc - 2, argv + 2);
        else
            return Selftest(argc - 1, argv + 1);
    } catch (string err) {
        cerr << "Exception: " << err << endl;
    } catch (const std::exception &exc) {
        cerr << "Exception: " << exc.what() << endl;
    } catch (...) {
        cerr << "Unknown exception" << endl;
    }

    return EXIT_FAILURE;
};
