#include <csignal>
#include <iostream>
#include <iomanip>

#include "cli.hpp"
#include "version.hpp"
#include "util/log.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <sys/ioctl.h>
}

using std::string;
using std::map;
using std::vector;

namespace {
// Command that is being executed, used for signal handling
ICmd *CurrentCmd = nullptr;

TCommandHandler *CurrentHandler = nullptr;

void SigInt(int sig) {
    if (CurrentCmd)
        CurrentCmd->Signal(sig);
}

void PrintAligned(const std::string &name, const std::string &desc,
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

template <typename Collection, typename MapFunction>
size_t MaxFieldLength(const Collection &coll, MapFunction mapper, size_t min = MIN_FIELD_LENGTH) {
    size_t len = 0;
    for (const auto &i : coll) {
        const auto length = mapper(i).length();
        if (length > len)
            len  = length;
    }

    return std::max(len, min) + 2;
}

class THelpCmd final : public ICmd {
    const bool UsagePrintData;
    TCommandHandler &Handler;

public:
    THelpCmd(TCommandHandler &handler, bool usagePrintData)
        : ICmd(&handler.GetPortoApi(), "help", 1,
               "[command]", "print help message for command"),
          UsagePrintData(usagePrintData),
          Handler(handler) {}

    void Usage();
    int Execute(int argc, char *argv[]) final override;
};

void THelpCmd::Usage() {
    int termWidth = 80;

    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        termWidth = w.ws_col;

    std::cerr << "Usage: " << program_invocation_short_name << " <command> [<args>]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Command list:" << std::endl;

    using CmdPair = TCommandHandler::RegisteredCommands::value_type;
    int nameWidth = MaxFieldLength(Handler.GetCommands(), [](const CmdPair &p) { return p.first; });

    for (const auto &i : Handler.GetCommands())
        PrintAligned(i.second->GetName(), i.second->GetDescription(), nameWidth, termWidth);

    std::cerr << std::endl << "Volume properties:" << std::endl;
    vector<TProperty> vlist;
    int ret = Api->ListVolumeProperties(vlist);
    if (ret) {
        PrintError("Unavailable");
    } else {
        nameWidth = MaxFieldLength(vlist, [](const TProperty &p) { return p.Name; });

        for (const auto& p : vlist)
            PrintAligned(p.Name, p.Description, nameWidth, termWidth);
    }

    std::cerr << std::endl << "Property list:" << std::endl;
    vector<TProperty> plist;
    ret = Api->Plist(plist);
    if (ret) {
        PrintError("Unavailable");
    } else {
        nameWidth = MaxFieldLength(vlist, [](const TProperty &p) { return p.Name; });

        for (const auto& p : plist)
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
        nameWidth = MaxFieldLength(dlist, [](const TData &d) { return d.Name; });

        for (const auto& d : dlist)
            PrintAligned(d.Name, d.Description, nameWidth, termWidth);
    }
    std::cerr << std::endl;
}

int THelpCmd::Execute(int argc, char *argv[]) {
    int ret = EXIT_FAILURE;

    if (argc == 0) {
        Usage();
        return ret;
    }

    const string name(argv[0]);
    const auto it = Handler.GetCommands().find(name);
    if (it == Handler.GetCommands().end()) {
        Usage();
    } else {
        std::cerr << "Usage: " << program_invocation_short_name
                  << " " << name << " " << it->second->GetUsage()
                  << std::endl;
        std::cerr << std::endl;
        std::cerr << it->second->GetDescription() << std::endl;
        std::cerr << it->second->GetHelp();
        ret = EXIT_SUCCESS;
    }
    
    return ret;
}
}  // namespace

size_t MaxFieldLength(const std::vector<std::string> &vec, size_t min) {
    return MaxFieldLength(vec, [](const string &s) { return s; }, min);
}

int ICmd::RunCmdImpl(const std::vector<std::string> &args,
                     std::unique_ptr<ICmd> cmd) {
    ICmd *prevCmd = CurrentCmd;

    std::vector<char *> cargs;
    std::vector<std::string> mutableArgs(args);
    cargs.push_back(program_invocation_name);
    for (auto& arg : mutableArgs)
        cargs.push_back(&arg[0]);

    cmd->SetDieOnSignal(false);

    CurrentCmd = cmd.get();
    int ret = cmd->Execute(cargs.size() - 1, (&cargs[0]) + 1);
    CurrentCmd = prevCmd;

    if (GotSignal())
        return InterruptedSignal;

    return ret;
}

ICmd::ICmd(TPortoAPI *api, const string& name, int args,
           const string& usage, const string& desc, const string& help) :
    Api(api), Name(name), Usage(usage), Desc(desc), Help(help), NeedArgs(args) {}

const string& ICmd::GetName() const { return Name; }
const string& ICmd::GetUsage() const { return Usage; }
const string& ICmd::GetDescription() const { return Desc; }
const string& ICmd::GetHelp() const { return Help; }

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

int TCommandHandler::TryExec(int argc, char *argv[]) {
    PORTO_ASSERT(argc > 1);
    const string name(argv[1]);

    const auto it = Commands.find(name);
    if (it == Commands.end()) {
        std::cerr << "Invalid command " << name << "!" << std::endl;
        return EXIT_FAILURE;
    }

    ICmd *cmd = it->second.get();
    PORTO_ASSERT(cmd);
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

TCommandHandler::TCommandHandler(TPortoAPI &api) : PortoApi(api) {
    PORTO_ASSERT(CurrentHandler == nullptr);
    CurrentHandler = this;
    RegisterCommand(std::unique_ptr<ICmd>(new THelpCmd(*this, true)));
}

TCommandHandler::~TCommandHandler() {
    PORTO_ASSERT(CurrentHandler == this);
    CurrentHandler = nullptr;
}

void TCommandHandler::RegisterCommand(std::unique_ptr<ICmd> cmd) {
    PORTO_ASSERT(cmd);
    Commands[cmd->GetName()] = std::move(cmd);
}

int TCommandHandler::HandleCommand(int argc, char *argv[]) {
    if (argc <= 1) {
        Usage(nullptr);
        return EXIT_FAILURE;
    }

    const string name(argv[1]);
    if (name == "-h" || name == "--help") {
        Usage(nullptr);
        return EXIT_FAILURE;
    }

    if (name == "-v" || name == "--version") {
        std::cerr << "client: " << GIT_TAG << " " << GIT_REVISION << std::endl;
        std::string tag, revision;
        int ret = PortoApi.GetVersion(tag, revision);

        if (!ret)
            std::cerr << "server: " << tag << " " << revision << std::endl;

        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = TryExec(argc, argv);
    } catch (const std::exception &exc) {
        std::cerr << exc.what() << std::endl;
    } catch (const string &err) {
        std::cerr << err << std::endl;
    } catch (const char *err) {
        std::cerr << err << std::endl;
    } catch (...) {
        std::cerr << "Got unknown error" << std::endl;
    }

    return ret;
}

void TCommandHandler::Usage(const char *command) {
    ICmd *cmd = Commands["help"].get();
    char *argv[] = { const_cast<char *>(command), nullptr };

    PORTO_ASSERT(cmd);
    cmd->Execute(command ? 1 : 0, argv);
}

int GetOpt(int argc, char *argv[], const std::vector<Option> &opts) {
    std::string optstring;
    for (const auto& o : opts) {
        optstring += o.key;
        if (o.hasArg)
            optstring += ":";
    }

    optind = 1;
    int opt;
    while ((opt = getopt(argc + 1, argv - 1, optstring.c_str())) != -1) {
        bool found = false;
        for (const auto& o : opts) {
            if (o.key == opt) {
                o.handler(optarg);
                found = true;
                break;
            }
        }

        if (!found) {
            PORTO_ASSERT(CurrentHandler);
            CurrentHandler->Usage(nullptr);
            exit(EXIT_FAILURE);
        }
    }

    return optind - 1;
}
