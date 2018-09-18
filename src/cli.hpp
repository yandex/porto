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
    TString Name, Usage, Desc, Help;
    sig_atomic_t Interrupted = 0;
public:
    int NeedArgs;

    ICmd(Porto::Connection *api, const TString &name, int args,
         const TString &usage, const TString &desc, const TString &help = "");
    virtual ~ICmd() {}
    const TString &GetName() const;
    const TString &GetUsage() const;
    const TString &GetDescription() const;
    const TString &GetHelp() const;

    void PrintError(const TString &prefix);
    void PrintError(const TString &prefix, const TError &error);
    void PrintUsage();
    bool ValidArgs(const std::vector<TString> &args);
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
    using RegisteredCommands = std::map<TString, std::unique_ptr<ICmd>>;

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
    const std::vector<TString> &Arguments;

    TCommandEnviroment() = delete;
    TCommandEnviroment(const TCommandEnviroment &) = delete;

public:
    int NeedArgs = 0;
    TCommandEnviroment(TCommandHandler &handler,
                       const std::vector<TString> &arguments)
        : Handler(handler),
          Arguments(arguments) {}

    TCommandEnviroment(TCommandEnviroment *env,
                       const std::vector<TString> &arguments)
        : Handler(env->Handler),
          Arguments(arguments) {}

    std::vector<TString> GetOpts(const std::vector<Option> &options);
    const std::vector<TString> &GetArgs() const { return Arguments; }
};

constexpr size_t MIN_FIELD_LENGTH = 8;
size_t MaxFieldLength(const std::vector<TString> &vec, size_t min = MIN_FIELD_LENGTH);
