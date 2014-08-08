#include <iostream>
#include <iomanip>
#include <sstream>

#include "libporto.hpp"

class ICmd {
protected:
    string name, usage, desc;
    int need_args;

    TPortoAPI api;

public:
    ICmd(const string& name, int args, const string& usage, const string& desc) :
        name(name), usage(usage), desc(desc), need_args(args) {}

    string& GetName() { return name; }
    string& GetUsage() { return usage; }
    string& GetDescription() { return desc; }

    bool ValidArgs(int argc, char *argv[]) {
        if (need_args < argc)
            return false;

        return true;
    }

    virtual int Execute(int argc, char *argv[]) = 0;
};

static vector<ICmd *> commands;

class THelpCmd : public ICmd {
public:
    THelpCmd() : ICmd("help", 1, "[command]", "print help message for command") {}

    void Usage(const char *name) {
        cout << "usage: " << name << " <command> [<args>]" << endl;
        cout << endl;
        cout << "list of commands:" << endl;
        for (ICmd *cmd : commands)
            cout << " " << left << setw(16) << cmd->GetName() << cmd->GetDescription() << endl;
    }

    int Execute(int argc, char *argv[]) {
        string name = argv[0];
        for (ICmd *cmd : commands) {
            if (cmd->GetName() == name) {
                cout << "usage: " << argv[0] << " " << name << " " << cmd->GetUsage() << endl;
                cout << endl;
                cout << cmd->GetDescription() << endl;

                return EXIT_SUCCESS;
            }
        }

        Usage(argv[0]);
        return EXIT_FAILURE;
    }
};

static void Usage(char *name, const char *command) {
    ICmd *cmd = new THelpCmd();
    char *argv[] = { name, (char *)"help", (char *)command, NULL };

    cmd->Execute(command ? 3 : 1, argv);
}

class TRawCmd : public ICmd {
public:
    TRawCmd() : ICmd("raw", 2, "<message>", "send raw protobuf message") {}

    int Execute(int argc, char *argv[]) {
        stringstream msg;

        std::vector<std::string> args(argv, argv + argc);
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

        string resp;
        if (!api.Raw(msg.str(), resp))
            cout << resp << endl;

        return 0;
    }
};

class TCreateCmd : public ICmd {
public:
    TCreateCmd() : ICmd("create", 1, "<name>", "create container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.Create(argv[0]);
        if (ret)
            cerr << "Can't create container, error = " << ret << endl;

        return ret;
    }
};

class TDestroyCmd : public ICmd {
public:
    TDestroyCmd() : ICmd("destroy", 1, "<name>", "destroy container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.Destroy(argv[0]);
        if (ret)
            cerr << "Can't destroy container, error = " << ret << endl;

        return ret;
    }
};

class TListCmd : public ICmd {
public:
    TListCmd() : ICmd("list", 0, "", "list created containers") {}

    int Execute(int argc, char *argv[])
    {
        vector<string> clist;
        int ret = api.List(clist);
        if (ret)
            cerr << "Can't list containers, error = " << ret << endl;
        else
            for (auto c : clist)
                cout << c << endl;

        return ret;
    }
};

class TPropertyListCmd : public ICmd {
public:
    TPropertyListCmd() : ICmd("plist", 1, "", "list supported container properties") {}

    int Execute(int argc, char *argv[])
    {
        vector<TProperty> plist;
        int ret = api.Plist(plist);
        if (ret)
            cerr << "Can't list properties, error = " << ret << endl;
        else
            for (auto p : plist)
                cout << p.name << " - " << p.description << endl;

        return ret;
    }
};

class TDataListCmd : public ICmd {
public:
    TDataListCmd() : ICmd("dlist", 1, "", "list supported container data") {}

    int Execute(int argc, char *argv[])
    {
        vector<TData> dlist;
        int ret = api.Dlist(dlist);
        if (ret)
            cerr << "Can't list data, error = " << ret << endl;
        else
            for (auto d : dlist)
                cout << d.name << " - " << d.description << endl;

        return ret;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    TGetPropertyCmd() : ICmd("get", 2, "<name> <property>", "get container property") {}

    int Execute(int argc, char *argv[])
    {
        vector<string> value;
        int ret = api.GetProperties(argv[0], argv[1], value);
        if (ret)
            cerr << "Can't get property, error = " << ret << endl;
        else
            for (auto v : value)
                cout << v << endl;

        return ret;
    }
};

class TSetPropertyCmd : public ICmd {
public:
    TSetPropertyCmd() : ICmd("set", 3, "<name> <property>", "set container property") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.SetProperty(argv[0], argv[1], argv[2]);
        if (ret)
            cerr << "Can't set property, error = " << ret << endl;

        return ret;
    }
};

class TGetDataCmd : public ICmd {
public:
    TGetDataCmd() : ICmd("data", 2, "<name> <data>", "get container data") {}

    int Execute(int argc, char *argv[])
    {
        vector<string> value;
        int ret = api.GetData(argv[0], argv[1], value);
        if (ret)
            cerr << "Can't get data, error = " << ret << endl;
        else
            for (auto v : value)
                cout << v << endl;

        return ret;
    }
};

class TStartCmd : public ICmd {
public:
    TStartCmd() : ICmd("start", 1, "<name>", "start container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.Start(argv[0]);
        if (ret)
            cerr << "Can't start container, error = " << ret << endl;

        return ret;
    }
};

class TStopCmd : public ICmd {
public:
    TStopCmd() : ICmd("stop", 1, "<name>", "stop container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.Stop(argv[0]);
        if (ret)
            cerr << "Can't stop container, error = " << ret << endl;

        return ret;
    }
};

class TPauseCmd : public ICmd {
public:
    TPauseCmd() : ICmd("pause", 1, "<name>", "pause container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.Pause(argv[0]);
        if (ret)
            cerr << "Can't pause container, error = " << ret << endl;

        return ret;
    }
};

class TResumeCmd : public ICmd {
public:
    TResumeCmd() : ICmd("resume", 1, "<name>", "resume container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = api.Resume(argv[0]);
        if (ret)
            cerr << "Can't resume container, error = " << ret << endl;

        return ret;
    }
};

int main(int argc, char *argv[])
{
    commands = {
        new THelpCmd(),
        new TCreateCmd(),
        new TDestroyCmd(),
        new TListCmd(),
        new TPropertyListCmd(),
        new TDataListCmd(),
        new TStartCmd(),
        new TStopCmd(),
        new TPauseCmd(),
        new TResumeCmd(),
        new TGetPropertyCmd(),
        new TSetPropertyCmd(),
        new TGetDataCmd(),
        new TRawCmd()
    };

    if (argc <= 1) {
        Usage(argv[0], NULL);
        return EXIT_FAILURE;
    }

    string name = argv[1];
    if (name == "-h" || name == "--help") {
        Usage(argv[0], NULL);
        return EXIT_FAILURE;
    }

    try {
        for (ICmd *cmd : commands)
            if (cmd->GetName() == name) {
                if (!cmd->ValidArgs(argc - 2, argv + 2)) {
                    Usage(argv[0], cmd->GetName().c_str());
                    return EXIT_FAILURE;
                }
                return cmd->Execute(argc, argv);
            }
    } catch (const char *err) {
        cerr << err << endl;
    }

    return EXIT_FAILURE;
};
