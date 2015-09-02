#pragma once

#include <memory>
#include <string>
#include <vector>
#include "libporto.hpp"

class ICmd {
protected:
    TPortoAPI *Api;
    std::string Name, Usage, Desc, Help;
    int NeedArgs;
    sig_atomic_t Interrupted = 0;
    bool DieOnSignal = false;

    int RunCmdImpl(const std::vector<std::string> &args,
                   std::unique_ptr<ICmd> command);

    template <typename T>
    int RunCmd(const std::vector<std::string> &args) {
        return RunCmdImpl(args, std::unique_ptr<T>(new T(Api)));
    }

public:
    int InterruptedSignal;

    bool GotSignal() const {
        return Interrupted;
    }

    ICmd(TPortoAPI *api, const std::string& name, int args,
         const std::string& usage, const std::string& desc, const std::string& help = "");
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
    bool ValidArgs(int argc, char *argv[]);
    void SetDieOnSignal(bool die);
    void Signal(int sig);
    virtual int Execute(int argc, char *argv[]) = 0;
};

class THelpCmd final : public ICmd {
    const bool UsagePrintData;
public:
    THelpCmd(TPortoAPI *api, bool usagePrintData);

    void Usage();
    int Execute(int argc, char *argv[]) override;
};

struct Option {
    char key;
    bool hasArg;
    std::function<void(const char *arg)> handler;
};

int GetOpt(int argc, char *argv[], const std::vector<Option> &opts);

size_t MaxFieldLength(const std::vector<std::string> &vec, size_t min = 8);
void RegisterCommand(ICmd *cmd);
int HandleCommand(TPortoAPI *api, int argc, char *argv[]);
