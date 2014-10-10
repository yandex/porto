#ifndef __CLI_HPP__
#define __CLI_HPP__

#include <string>
#include "libporto.hpp"

class ICmd {
protected:
    TPortoAPI *Api;
    std::string Name, Usage, Desc;
    int NeedArgs;

public:
    ICmd(TPortoAPI *api, const std::string& name, int args, const std::string& usage, const std::string& desc);
    std::string &GetName();
    std::string &GetUsage();
    std::string &GetDescription();

    const std::string &ErrorName(int err);
    void PrintError(const TError &error, const std::string &str);
    void PrintError(const std::string &str);
    bool ValidArgs(int argc, char *argv[]);
    virtual int Execute(int argc, char *argv[]) = 0;
    virtual void Signal(int sig) {}
};

class THelpCmd : public ICmd {
    bool UsagePrintData;
public:
    THelpCmd(TPortoAPI *api, bool usagePrintData);

    void Usage();
    int Execute(int argc, char *argv[]);
};

void RegisterCommand(ICmd *cmd);
int HandleCommand(int argc, char *argv[]);

#endif
