#pragma once

#include <memory>
#include <string>
#include <vector>
#include "libporto.hpp"

class TCommandEnviroment;

class ICmd {
protected:
    TPortoAPI *Api;
    std::string Name, Usage, Desc, Help;
    size_t NeedArgs;
    sig_atomic_t Interrupted = 0;
    bool DieOnSignal = false;

    int RunCmdImpl(const std::vector<std::string> &args,
                   std::unique_ptr<ICmd> command,
                   TCommandEnviroment *env);

public:
    int InterruptedSignal;

    bool GotSignal() const {
        return Interrupted;
    }

    ICmd(TPortoAPI *api, const std::string &name, int args,
         const std::string &usage, const std::string &desc, const std::string &help = "");
    virtual ~ICmd() {}
    const std::string &GetName() const;
    const std::string &GetUsage() const;
    const std::string &GetDescription() const;
    const std::string &GetHelp() const;

    const std::string &ErrorName(int err);
    void Print(const std::string &val);
    void PrintPair(const std::string &key, const std::string &val);
    void PrintError(const TError &error, const std::string &str);
    void PrintError(const std::string &str);
    bool ValidArgs(const std::vector<std::string> &args);
    void SetDieOnSignal(bool die);
    void Signal(int sig);
    virtual int Execute(TCommandEnviroment *env) = 0;

    template <typename T>
    int RunCmd(const std::vector<std::string> &args, TCommandEnviroment *env) {
        return RunCmdImpl(args, std::unique_ptr<T>(new T(Api)), env);
    }
};

struct Option {
    char key;
    bool hasArg;
    std::function<void(const char *arg)> handler;
};

class TCommandHandler {
    void operator=(const TCommandHandler&) = delete;
    TCommandHandler(const TCommandHandler&) = delete;

    int TryExec(const std::string &commandName, const std::vector<std::string> &commandArgs);

public:
    using RegisteredCommands = std::map<std::string, std::unique_ptr<ICmd>>;

    explicit TCommandHandler(TPortoAPI &api);
    ~TCommandHandler();

    void RegisterCommand(std::unique_ptr<ICmd> cmd);
    int HandleCommand(int argc, char *argv[]);
    void Usage(const char *command);

    TPortoAPI &GetPortoApi() { return PortoApi; }
    const RegisteredCommands &GetCommands() const { return Commands; }

private:
    RegisteredCommands Commands;
    TPortoAPI &PortoApi;
};

class TCommandEnviroment {
    TCommandHandler &Handler;
    const std::vector<std::string> &Arguments;

    TCommandEnviroment() = delete;
    TCommandEnviroment(const TCommandEnviroment &) = delete;

public:
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
