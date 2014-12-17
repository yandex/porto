#include <iostream>
#include <iomanip>

#include "cli.hpp"
#include "util/unix.hpp"

using std::string;
using std::map;
using std::vector;

ICmd::ICmd(TPortoAPI *api, const string& name, int args, const string& usage, const string& desc) :
    Api(api), Name(name), Usage(usage), Desc(desc), NeedArgs(args) {}

    string& ICmd::GetName() { return Name; }
    string& ICmd::GetUsage() { return Usage; }
    string& ICmd::GetDescription() { return Desc; }

    const string &ICmd::ErrorName(int err) {
        if (err == INT_MIN) {
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

static map<string, ICmd *> commands;

THelpCmd::THelpCmd(TPortoAPI *api, bool usagePrintData) : ICmd(api, "help", 1, "[command]", "print help message for command"), UsagePrintData(usagePrintData) {}

void THelpCmd::Usage() {
    const int nameWidth = 32;

    std::cerr << "Usage: " << program_invocation_short_name << " <command> [<args>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Command list:" << std::endl;
    for (auto i : commands)
        std::cerr << " " << std::left << std::setw(nameWidth) << i.second->GetName() << i.second->GetDescription() << std::endl;

    int ret;
    std::cerr << std::endl << "Property list:" << std::endl;
    vector<TProperty> plist;
    ret = Api->Plist(plist);
    if (ret) {
        PrintError("Unavailable");
    } else
        for (auto p : plist)
            std::cerr << " " << std::left << std::setw(nameWidth) << p.Name
                << p.Description << std::endl;

    if (!UsagePrintData)
        return;

    std::cerr << std::endl << "Data list:" << std::endl;
    vector<TData> dlist;
    ret = Api->Dlist(dlist);
    if (ret)
        PrintError("Unavailable");
    else
        for (auto d : dlist)
            std::cerr << " " << std::left << std::setw(nameWidth) << d.Name
                << d.Description << std::endl;
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

ICmd *currentCmd;

static void TryExec(int argc, char *argv[]) {
    string name(argv[1]);

    if (commands.find(name) == commands.end())
        return;

    ICmd *cmd = commands[name];
    if (!cmd->ValidArgs(argc - 2, argv + 2)) {
        Usage(cmd->GetName().c_str());
        exit(EXIT_FAILURE);
    }

    currentCmd = cmd;
    exit(cmd->Execute(argc - 2, argv + 2));
}

void SigInt(int sig) {
    (void)RegisterSignal(sig, SIG_DFL);
    currentCmd->Signal(sig);
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
        std::string tag, revision;
        int ret = api->GetVersion(tag, revision);

        std::cerr << "client: " << GIT_TAG << " " << GIT_REVISION << std::endl;
        if (!ret)
            std::cerr << "server: " << tag << " " << revision << std::endl;

        return EXIT_FAILURE;
    }

    // in case client closes pipe we are writing to in the protobuf code
    (void)RegisterSignal(SIGPIPE, SIG_IGN);
    (void)RegisterSignal(SIGINT, SigInt);

    try {
        // porto <command> <arg2> <arg2>
        TryExec(argc, argv);

#if 0
        // porto <arg1> <command> <arg2>
        if (argc >= 2) {
            char *p = argv[1];
            argv[1] = argv[2];
            argv[2] = p;

            TryExec(argc, argv);
        }
#endif

        std::cerr << "Invalid command " << name << "!" << std::endl;
    } catch (string err) {
        std::cerr << err << std::endl;
    }

    return EXIT_FAILURE;
}
