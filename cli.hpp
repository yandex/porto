#pragma once

#include <string>
#include <csignal>
#include "libporto.hpp"

class ICmd {
protected:
    TPortoAPI *Api;
    std::string Name, Usage, Desc;
    int NeedArgs;
    sig_atomic_t Interrupted = 0;
    int InterruptedSignal;

public:
    ICmd(TPortoAPI *api, const std::string& name, int args, const std::string& usage, const std::string& desc);
    std::string &GetName();
    std::string &GetUsage();
    std::string &GetDescription();

    const std::string &ErrorName(int err);
    void Print(const std::string &val);
    void PrintPair(const std::string &key, const std::string &val);
    void PrintError(const TError &error, const std::string &str);
    void PrintError(const std::string &str);
    bool ValidArgs(int argc, char *argv[]);
    virtual int Execute(int argc, char *argv[]) = 0;
    virtual void Signal(int sig);
};

class THelpCmd : public ICmd {
    bool UsagePrintData;
public:
    THelpCmd(TPortoAPI *api, bool usagePrintData);

    void Usage();
    int Execute(int argc, char *argv[]);
};

int GetOpt(int argc, char *argv[],
           const std::map<char, std::function<void()>> &opts);

size_t MaxFieldLength(std::vector<std::string> &vec, size_t min = 8);
void RegisterCommand(ICmd *cmd);
int HandleCommand(TPortoAPI *api, int argc, char *argv[]);
