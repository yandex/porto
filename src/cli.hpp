#pragma once

#include <string>
#include "libporto.hpp"

// Command that is being executed, used for signal handling
class ICmd;
extern ICmd *CurrentCmd;

class ICmd {
protected:
    TPortoAPI *Api;
    std::string Name, Usage, Desc, Help;
    int NeedArgs;
    sig_atomic_t Interrupted = 0;
    bool DieOnSignal = false;

    template <typename T>
    int RunCmd(const std::vector<std::string> &args) {
        ICmd *prevCmd = CurrentCmd;

        std::vector<char *> cargs;
        cargs.push_back(strdup(program_invocation_name));
        for (auto arg : args)
            cargs.push_back(strdup(arg.c_str()));

        auto cmd = new T(Api);
        cmd->SetDieOnSignal(false);

        CurrentCmd = cmd;
        int ret = cmd->Execute(cargs.size() - 1, ((char **)cargs.data()) + 1);
        CurrentCmd = prevCmd;

        for (auto arg : cargs)
            free(arg);

        delete cmd;

        if (GotSignal())
            return InterruptedSignal;

        return ret;
    }

public:
    int InterruptedSignal;

    bool GotSignal() {
        return Interrupted;
    }

    ICmd(TPortoAPI *api, const std::string& name, int args,
         const std::string& usage, const std::string& desc, const std::string& help = "");
    virtual ~ICmd() {}
    std::string &GetName();
    std::string &GetUsage();
    std::string &GetDescription();
    std::string &GetHelp();

    const std::string &ErrorName(int err);
    void Print(const std::string &val);
    void PrintPair(const std::string &key, const std::string &val);
    void PrintError(const TError &error, const std::string &str);
    void PrintError(const std::string &str);
    bool ValidArgs(int argc, char *argv[]);
    void SetDieOnSignal(bool die);
    void Signal(int sig);
    virtual int Execute(int argc, char *argv[]) = 0;
};

class THelpCmd : public ICmd {
    bool UsagePrintData;
public:
    THelpCmd(TPortoAPI *api, bool usagePrintData);

    void Usage();
    int Execute(int argc, char *argv[]);
};

struct Option {
    char key;
    bool hasArg;
    std::function<void(const char *arg)> handler;
};

int GetOpt(int argc, char *argv[], const std::vector<Option> &opts);

size_t MaxFieldLength(std::vector<std::string> &vec, size_t min = 8);
void RegisterCommand(ICmd *cmd);
int HandleCommand(TPortoAPI *api, int argc, char *argv[]);
