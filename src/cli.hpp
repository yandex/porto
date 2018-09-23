#pragma once

#include <functional>
#include <memory>
#include <string>
#include <csignal>
#include <vector>

#include "libporto.hpp"
#include "util/error.hpp"

class TCommandEnviroment;

class ICmd {
protected:
    Porto::Connection *Api;
    std::string Name, Usage, Desc, Help;
    sig_atomic_t Interrupted = 0;
public:
    int NeedArgs;

    ICmd(Porto::Connection *api, const std::string &name, int args,
         const std::string &usage, const std::string &desc, const std::string &help = "");
    virtual ~ICmd() {}
    const std::string &GetName() const;
    const std::string &GetUsage() const;
    const std::string &GetDescription() const;
    const std::string &GetHelp() const;

    void PrintError(const std::string &prefix);
    void PrintError(const std::string &prefix, const TError &error);
    void PrintUsage();
    bool ValidArgs(const std::vector<std::string> &args);
    virtual int Execute(TCommandEnviroment *env) = 0;
};

struct Option {
    char key;
    bool hasArg;
    std::function<void(const char *arg)> handler;
};

class TCommandHandler {
    void operator=(const TCommandHandler&) = delete;
    TCommandHandler(const TCommandHandler&) = delete;

public:
    using RegisteredCommands = std::map<std::string, std::unique_ptr<ICmd>>;

    explicit TCommandHandler(Porto::Connection &api);
    ~TCommandHandler();

    void RegisterCommand(std::unique_ptr<ICmd> cmd);
    int HandleCommand(int argc, char *argv[]);
    void Usage(const char *command);

    Porto::Connection &GetPortoApi() { return PortoApi; }
    const RegisteredCommands &GetCommands() const { return Commands; }

    template <typename TCommand>
    void RegisterCommand() {
        RegisterCommand(std::unique_ptr<ICmd>(new TCommand(&PortoApi)));
    }

private:
    RegisteredCommands Commands;
    Porto::Connection &PortoApi;
};

class TCommandEnviroment {
    TCommandHandler &Handler;
    const std::vector<std::string> &Arguments;

    TCommandEnviroment() = delete;
    TCommandEnviroment(const TCommandEnviroment &) = delete;

public:
    int NeedArgs = 0;
    TCommandEnviroment(TCommandHandler &handler,
                       const std::vector<std::string> &arguments)
        : Handler(handler),
          Arguments(arguments) {}

    TCommandEnviroment(TCommandEnviroment *env,
                       const std::vector<std::string> &arguments)
        : Handler(env->Handler),
          Arguments(arguments) {}

    std::vector<std::string> GetOpts(const std::vector<Option> &options);
    const std::vector<std::string> &GetArgs() const { return Arguments; }
};

constexpr size_t MIN_FIELD_LENGTH = 8;
size_t MaxFieldLength(const std::vector<std::string> &vec, size_t min = MIN_FIELD_LENGTH);
