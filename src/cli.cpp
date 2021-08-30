#include <csignal>
#include <climits>

#include "cli.hpp"
#include "version.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "fmt/format.h"

extern "C" {
#include <unistd.h>
#include <sys/ioctl.h>
}

using std::string;
using std::map;
using std::vector;

namespace {

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

    fmt::print("  {:<{}}{}\n", name, nameWidth, v[0]);

    for (size_t i = 1; i < v.size(); i++)
        fmt::print("  {:<{}}{}\n", "", nameWidth, v[i]);
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
    TCommandHandler &Handler;

public:
    THelpCmd(TCommandHandler &handler)
        : ICmd(&handler.GetPortoApi(), "help", 1,
               "[command]", "print help message for command"),
          Handler(handler) {}

    void Usage();
    int Execute(TCommandEnviroment *env) final override;
};

void THelpCmd::Usage() {
    int termWidth = 80;

    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        termWidth = w.ws_col;

    fmt::print("Usage: {} [option]... <command> [argument]...\n"
               "\n"
               "Options:\n"
               "  -t, --timeout=<seconds>    rpc timeout ({} seconds)\n"
               "  -h, --help\n"
               "  -v, --version\n"
               "\n"
               "Commands:\n",
               program_invocation_short_name,
               Api->GetTimeout());

    using CmdPair = TCommandHandler::RegisteredCommands::value_type;
    int nameWidth = MaxFieldLength(Handler.GetCommands(), [](const CmdPair &p) { return p.first; });

    for (const auto &i : Handler.GetCommands())
        PrintAligned(i.second->GetName(), i.second->GetDescription(), nameWidth, termWidth);

    fmt::print("\nVolume properties:\n");
    vector<Porto::Property> vlist;
    int ret = Api->ListVolumeProperties(vlist);
    if (ret) {
        PrintError("Volume properties unavailable");
    } else {
        nameWidth = MaxFieldLength(vlist, [](const Porto::Property &p) { return p.Name; });

        for (const auto &p : vlist)
            PrintAligned(p.Name, p.Description, nameWidth, termWidth);
    }

    fmt::print("\nContainer properties:\n");
    vector<Porto::Property> plist;
    ret = Api->ListProperties(plist);
    if (ret) {
        PrintError("Properties unavailable");
    } else {
        nameWidth = MaxFieldLength(plist, [](const Porto::Property &p) { return p.Name; });

        for (const auto &p : plist)
            PrintAligned(p.Name, p.Description, nameWidth, termWidth);
    }

    fmt::print("\n");
}

int THelpCmd::Execute(TCommandEnviroment *env) {
    int ret = EXIT_FAILURE;
    const auto &args = env->GetArgs();

    if (args.empty()) {
        Usage();
        return ret;
    }

    const std::string &name = args[0];
    const auto it = Handler.GetCommands().find(name);
    if (it == Handler.GetCommands().end()) {

        std::string helper = fmt::format("{}/{}-{}", PORTO_HELPERS_PATH, program_invocation_short_name, name);
        execl(helper.c_str(), helper.c_str(), "--help", nullptr);

        helper = fmt::format("{}-{}", program_invocation_name, name);
        execlp(helper.c_str(), helper.c_str(), "--help", nullptr);

        Usage();
    } else {
        it->second->PrintUsage();
        ret = EXIT_SUCCESS;
    }
    return ret;
}
}  // namespace

size_t MaxFieldLength(const std::vector<std::string> &vec, size_t min) {
    return MaxFieldLength(vec, [](const string &s) { return s; }, min);
}

ICmd::ICmd(Porto::Connection *api, const string &name, int args,
           const string &usage, const string &desc, const string &help) :
    Api(api), Name(name), Usage(usage), Desc(desc), Help(help), NeedArgs(args) {}

const string &ICmd::GetName() const { return Name; }
const string &ICmd::GetUsage() const { return Usage; }
const string &ICmd::GetDescription() const { return Desc; }
const string &ICmd::GetHelp() const { return Help; }

void ICmd::PrintError(const string &prefix) {
    fmt::print(stderr, "{}: {}\n", prefix, Api->GetLastError());
}

void ICmd::PrintError(const string &prefix, const TError &error) {
    fmt::print(stderr, "{}: {}\n", prefix, error.ToString());
}

void ICmd::PrintUsage() {
    fmt::print("Usage: {} {} {}\n\n{}\n\n{}\n",
               program_invocation_short_name, Name, Usage,
               Desc,
               Help);
}

bool ICmd::ValidArgs(const std::vector<std::string> &args) {
    if ((int)args.size() < NeedArgs)
        return false;

    if (args.size() >= 1) {
        const string &arg = args[0];
        if (arg == "-h" || arg == "--help" || arg == "help")
            return false;;
    }

    return true;
}

TCommandHandler::TCommandHandler(Porto::Connection &api) : PortoApi(api) {
    RegisterCommand(std::unique_ptr<ICmd>(new THelpCmd(*this)));
}

TCommandHandler::~TCommandHandler() {
}

void TCommandHandler::RegisterCommand(std::unique_ptr<ICmd> cmd) {
    Commands[cmd->GetName()] = std::move(cmd);
}

int TCommandHandler::HandleCommand(int argc, char *argv[]) {

    if (argc > 2 && (std::string(argv[1]) == "--disk-timeout")) {
        PortoApi.SetDiskTimeout(atoi(argv[2]));
        argc -= 2;
        argv += 2;
    }

    if (argc > 2 && (std::string(argv[1]) == "-t" ||
                     std::string(argv[1]) == "--timeout")) {
        PortoApi.SetTimeout(atoi(argv[2]));
        argc -= 2;
        argv += 2;
    }

    setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc <= 1) {
        Usage(nullptr);
        return EXIT_FAILURE;
    }

    const std::string name(argv[1]);
    if (name == "-h" || name == "--help") {
        Usage(nullptr);
        return EXIT_FAILURE;
    }

    if (name == "-v" || name == "--version") {
        std::string version, revision;

        fmt::print("client: {} {}\n", PORTO_VERSION, PORTO_REVISION);

        if (PortoApi.GetVersion(version, revision))
            return EXIT_FAILURE;

        fmt::print("server: {} {}\n", version, revision);
        return EXIT_SUCCESS;
    }

    const auto it = Commands.find(name);
    if (it == Commands.end()) {

        std::string helper = fmt::format("{}/{}-{}", PORTO_HELPERS_PATH, program_invocation_short_name, name);
        argv[1] = (char *)helper.c_str();
        execv(argv[1], argv + 1);

        helper = fmt::format("{}-{}", program_invocation_name, name);
        argv[1] = (char *)helper.c_str();
        execvp(argv[1], argv + 1);

        fmt::print(stderr,"Invalid command: {}\n", name);
        return EXIT_FAILURE;
    }

    const std::vector<std::string> commandArgs(argv + 2, argv + argc);
    ICmd *cmd = it->second.get();
    if (!cmd->ValidArgs(commandArgs)) {
        Usage(cmd->GetName().c_str());
        return EXIT_FAILURE;
    }

    // in case client closes pipe we are writing to in the protobuf code
    Signal(SIGPIPE, SIG_IGN);

    TCommandEnviroment commandEnv{*this, commandArgs};
    commandEnv.NeedArgs = cmd->NeedArgs;
    return cmd->Execute(&commandEnv);
}

void TCommandHandler::Usage(const char *command) {
    ICmd *cmd = Commands["help"].get();

    std::vector<std::string> args;
    if (command)
        args.push_back(command);
    TCommandEnviroment commandEnv{*this, args};
    cmd->Execute(&commandEnv);
}

vector<string> TCommandEnviroment::GetOpts(const vector<Option> &options) {
    std::string optstring = "+";
    for (const auto &o : options) {
        optstring += o.key;
        if (o.hasArg)
            optstring += ":";
    }

    int opt;
    vector<string> mutableBuffer = Arguments;
    vector<const char*> rawArgs;
    rawArgs.reserve(mutableBuffer.size() + 2);
    std::string fakeCmd = "portoctl";
    rawArgs.push_back(fakeCmd.c_str());
    for (auto &arg : mutableBuffer)
        rawArgs.push_back(arg.c_str());
    rawArgs.push_back(nullptr);
    optind = 0;
    while ((opt = getopt(rawArgs.size() - 1, (char* const*)rawArgs.data(), optstring.c_str())) != -1) {
        bool found = false;
        for (const auto &o : options) {
            if (o.key == opt) {
                o.handler(optarg);
                found = true;
                break;
            }
        }

        if (!found) {
            Handler.Usage(nullptr);
            exit(EXIT_FAILURE);
        }
    }

    if ((int)Arguments.size() - optind + 1 < NeedArgs) {
            Handler.Usage(nullptr);
            exit(EXIT_FAILURE);
    }

    return vector<string>(Arguments.begin() + optind - 1, Arguments.end());
}
