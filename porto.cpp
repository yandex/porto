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

        if (argc >= 1) {
            string arg(argv[0]);
            if (arg == "-h" || arg == "--help" || arg == "help")
                return false;;
        }

        return true;
    }

    virtual int Execute(int argc, char *argv[]) = 0;
};

static vector<ICmd *> commands;

class THelpCmd : public ICmd {
public:
    THelpCmd() : ICmd("help", 1, "[command]", "print help message for command") {}

    void Usage()
    {
        cout << "usage: " << program_invocation_short_name << " <command> [<args>]" << endl;
        cout << endl;
        cout << "list of commands:" << endl;
        for (ICmd *cmd : commands)
            cout << " " << left << setw(16) << cmd->GetName() << cmd->GetDescription() << endl;
    }

    int Execute(int argc, char *argv[])
    {
        if (argc == 0) {
            Usage();
            return EXIT_FAILURE;
        }

        string name(argv[0]);
        for (ICmd *cmd : commands) {
            if (cmd->GetName() == name) {
                cout << "usage: " << program_invocation_short_name << " " << name << " " << cmd->GetUsage() << endl;
                cout << endl;
                cout << cmd->GetDescription() << endl;

                return EXIT_SUCCESS;
            }
        }

        Usage();
        return EXIT_FAILURE;
    }
};

static void Usage(const char *command) {
    ICmd *cmd = new THelpCmd();
    char *argv[] = { (char *)command, NULL };

    cmd->Execute(command ? 1 : 0, argv);
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
            for (auto c : clist) {
                string s;
                ret = api.GetData(c, "state", s);
                if (ret)
                    cerr << "Can't get container state, error = " << ret << endl;
                cout << left << setw(40) << c
                     << setw(40) << s << endl;

            }

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
                cout << left << setw(40) << p.name
                     << setw(40) << p.description << endl;

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
                cout << left << setw(40) << d.name
                     << setw(40) << d.description << endl;

        return ret;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    TGetPropertyCmd() : ICmd("get", 2, "<name> <property>", "get container property") {}

    int Execute(int argc, char *argv[])
    {
        string value;
        int ret = api.GetProperty(argv[0], argv[1], value);
        if (ret)
            cerr << "Can't get property, error = " << ret << endl;
        else
            cout << value << endl;

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
        string value;
        int ret = api.GetData(argv[0], argv[1], value);
        if (ret)
            cerr << "Can't get data, error = " << ret << endl;
        else
            cout << value << endl;

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
        Usage(NULL);
        return EXIT_FAILURE;
    }

    string name(argv[1]);
    if (name == "-h" || name == "--help") {
        Usage(NULL);
        return EXIT_FAILURE;
    }

    try {
        for (ICmd *cmd : commands)
            if (cmd->GetName() == name) {
                if (!cmd->ValidArgs(argc - 2, argv + 2)) {
                    Usage(cmd->GetName().c_str());
                    return EXIT_FAILURE;
                }

                return cmd->Execute(argc - 2, argv + 2);
            }
    } catch (const char *err) {
        cerr << err << endl;
    }

    return EXIT_FAILURE;
};
