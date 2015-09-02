#include <csignal>
#include <iostream>
#include <iomanip>

#include "cli.hpp"
#include "version.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <sys/ioctl.h>
}

using std::string;
using std::map;
using std::vector;

ICmd *CurrentCmd;

size_t MaxFieldLength(std::vector<std::string> &vec, size_t min) {
    size_t len = 0;
    for (auto &i : vec)
        if (i.length() > len)
            len  = i.length();

    return (len > min ? len : min) + 2;
}

ICmd::ICmd(TPortoAPI *api, const string& name, int args,
           const string& usage, const string& desc, const string& help) :
    Api(api), Name(name), Usage(usage), Desc(desc), Help(help), NeedArgs(args) {}

    string& ICmd::GetName() { return Name; }
    string& ICmd::GetUsage() { return Usage; }
    string& ICmd::GetDescription() { return Desc; }
    string& ICmd::GetHelp() { return Help; }

    const string &ICmd::ErrorName(int err) {
        if (err == INT_MAX) {
            static const string err = "portod unavailable";
            return err;
        }
        return rpc::EError_Name(static_cast<rpc::EError>(err));
    }

void ICmd::Print(const std::string &val) {
    std::cout << val;

    if (!val.length() || val[val.length() - 1] != '\n')
        std::cout << std::endl;
}

void ICmd::PrintPair(const std::string &key, const std::string &val) {
    Print(key + " = " + val);
}

void ICmd::PrintError(const TError &error, const string &str) {
    if (error.GetMsg().length())
        std::cerr << str << ": " << ErrorName(error.GetError()) << " (" << error.GetMsg() << ")" << std::endl;
    else
        std::cerr << str << ": " << ErrorName(error.GetError()) << std::endl;
}

void ICmd::PrintError(const string &str) {
    int num;
    string msg;

    Api->GetLastError(num, msg);

    TError error((EError)num, msg);
    PrintError(error, str);
}

bool ICmd::ValidArgs(int argc, char *argv[]) {
    if (argc < NeedArgs)
        return false;

    if (argc >= 1) {
        string arg(argv[0]);
        if (arg == "-h" || arg == "--help" || arg == "help")
            return false;;
    }

    return true;
}

void ICmd::SetDieOnSignal(bool die) {
    DieOnSignal = die;
}

void ICmd::Signal(int sig) {
    Interrupted = 1;
    InterruptedSignal = sig;
    if (DieOnSignal) {
        // TODO: don't raise signal here, try to handle in each command
        ResetAllSignalHandlers();
        raise(sig);
    }
}

static map<string, ICmd *> commands;

THelpCmd::THelpCmd(TPortoAPI *api, bool usagePrintData) : ICmd(api, "help", 1, "[command]", "print help message for command"), UsagePrintData(usagePrintData) {}

static void PrintAligned(const std::string &name, const std::string &desc,
                         const size_t nameWidth, const size_t termWidth) {
    std::vector<std::string> v;
    size_t descWidth = termWidth - nameWidth - 4;

    size_t start = 0;
    for (size_t i = 0; i < desc.length(); i++) {
        if (i - start > descWidth) {
            v.push_back(std::string(desc, start, i - start));
            start = i;
        }
    }
    std::string last = std::string(desc, start, desc.length());
    if (last.length())
        v.push_back(last);

    std::cerr << "  " << std::left << std::setw(nameWidth) << name
        << v[0] << std::endl;
    for (size_t i = 1; i < v.size(); i++)
        std::cerr << "  " << std::left << std::setw(nameWidth) << " "
            << v[i] << std::endl;
}


void THelpCmd::Usage() {
    int nameWidth;
    int termWidth = 80;

    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        termWidth = w.ws_col;

    std::cerr << "Usage: " << program_invocation_short_name << " <command> [<args>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Command list:" << std::endl;

    std::vector<std::string> tmpVec;
    for (auto i : commands)
        tmpVec.push_back(i.first);
    nameWidth = MaxFieldLength(tmpVec);

    for (auto i : commands)
        PrintAligned(i.second->GetName(), i.second->GetDescription(), nameWidth, termWidth);

    int ret;

    std::cerr << std::endl << "Volume properties:" << std::endl;
    vector<TProperty> vlist;
    ret = Api->ListVolumeProperties(vlist);
    if (ret) {
        PrintError("Unavailable");
    } else {
        tmpVec.clear();
        for (auto p : vlist)
            tmpVec.push_back(p.Name);
        nameWidth = MaxFieldLength(tmpVec);

        for (auto p : vlist)
            PrintAligned(p.Name, p.Description, nameWidth, termWidth);
    }

    std::cerr << std::endl << "Property list:" << std::endl;
    vector<TProperty> plist;
    ret = Api->Plist(plist);
    if (ret) {
        PrintError("Unavailable");
    } else {
        tmpVec.clear();
        for (auto p : plist)
            tmpVec.push_back(p.Name);
        nameWidth = MaxFieldLength(tmpVec);

        for (auto p : plist)
            PrintAligned(p.Name, p.Description, nameWidth, termWidth);
    }

    if (!UsagePrintData)
        return;

    std::cerr << std::endl << "Data list:" << std::endl;
    vector<TData> dlist;
    ret = Api->Dlist(dlist);
    if (ret) {
        PrintError("Unavailable");
    } else {
        tmpVec.clear();
        for (auto d : dlist)
            tmpVec.push_back(d.Name);
        nameWidth = MaxFieldLength(tmpVec);

        for (auto d : dlist)
            PrintAligned(d.Name, d.Description, nameWidth, termWidth);
    }
    std::cerr << std::endl;
}

int THelpCmd::Execute(int argc, char *argv[]) {
    if (argc == 0) {
        Usage();
        return EXIT_FAILURE;
    }

    string name(argv[0]);
    for (auto i : commands) {
        if (i.second->GetName() == name) {
            std::cerr << "Usage: " << program_invocation_short_name << " " << name << " " << i.second->GetUsage() << std::endl;
            std::cerr << std::endl;
            std::cerr << i.second->GetDescription() << std::endl;
            std::cerr << i.second->GetHelp();
            return EXIT_SUCCESS;
        }
    }

    Usage();
    return EXIT_FAILURE;
}

static void Usage(const char *command) {
    ICmd *cmd = commands["help"];
    char *argv[] = { (char *)command, NULL };

    cmd->Execute(command ? 1 : 0, argv);
}

void RegisterCommand(ICmd *cmd) {
    commands[cmd->GetName()] = cmd;
}

void SigInt(int sig) {
    if (CurrentCmd)
        CurrentCmd->Signal(sig);
}

static int TryExec(int argc, char *argv[]) {
    string name(argv[1]);

    if (commands.find(name) == commands.end()) {
        std::cerr << "Invalid command " << name << "!" << std::endl;
        return EXIT_FAILURE;
    }

    ICmd *cmd = commands[name];
    if (!cmd->ValidArgs(argc - 2, argv + 2)) {
        Usage(cmd->GetName().c_str());
        return EXIT_FAILURE;
    }

    CurrentCmd = cmd;

    // in case client closes pipe we are writing to in the protobuf code
    (void)RegisterSignal(SIGPIPE, SIG_IGN);
    (void)RegisterSignal(SIGINT, SigInt);
    (void)RegisterSignal(SIGTERM, SigInt);

    int ret = cmd->Execute(argc - 2, argv + 2);
    if (cmd->GotSignal())
        return -cmd->InterruptedSignal;
    else
        return ret;
}

int HandleCommand(TPortoAPI *api, int argc, char *argv[]) {
    if (argc <= 1) {
        Usage(NULL);
        return EXIT_FAILURE;
    }

    string name(argv[1]);
    if (name == "-h" || name == "--help") {
        Usage(NULL);
        return EXIT_FAILURE;
    }

    if (name == "-v" || name == "--version") {
        std::cerr << "client: " << GIT_TAG << " " << GIT_REVISION << std::endl;
        std::string tag, revision;
        int ret = api->GetVersion(tag, revision);

        if (!ret)
            std::cerr << "server: " << tag << " " << revision << std::endl;

        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = TryExec(argc, argv);
    } catch (string err) {
        std::cerr << err << std::endl;
    } catch (const char *err) {
        std::cerr << err << std::endl;
    } catch (...) {
        std::cerr << "Got unknown error" << std::endl;
    }

    for (auto pair : commands)
        delete pair.second;

    return ret;
}

int GetOpt(int argc, char *argv[], const std::vector<Option> &opts) {
    std::string optstring;
    for (auto o : opts) {
        optstring += o.key;
        if (o.hasArg)
            optstring += ":";
    }

    optind = 1;
    int opt;
    while ((opt = getopt(argc + 1, argv - 1, optstring.c_str())) != -1) {
        bool found = false;
        for (auto o : opts) {
            if (o.key == opt) {
                o.handler(optarg);
                found = true;
                break;
            }
        }

        if (!found) {
            Usage(NULL);
            exit(EXIT_FAILURE);
        }
    }

    return optind - 1;
}
