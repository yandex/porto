#include <iostream>
#include <iomanip>
#include <sstream>

#include "version.hpp"
#include "libporto.hpp"

using namespace std;

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

    const string &ErrorName(int err) {
        return rpc::EContainerError_Name(static_cast<rpc::EContainerError>(err));
    }

    bool ValidArgs(int argc, char *argv[]) {
        if (argc < need_args)
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

        int ret;
        cout << endl << "list of properties:" << endl;
        vector<TProperty> plist;
        ret = api.Plist(plist);
        if (ret)
            cerr << "Can't list properties, error = " << ErrorName(ret) << endl;
        else
            for (auto p : plist)
                cout << " " << left << setw(16) << p.name
                     << setw(40) << p.description << endl;

        cout << endl << "list of data:" << endl;
        vector<TData> dlist;
        ret = api.Dlist(dlist);
        if (ret)
            cerr << "Can't list data, error = " << ErrorName(ret) << endl;
        else
            for (auto d : dlist)
                cout << " " << left << setw(16) << d.name
                     << setw(40) << d.description << endl;
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
            cerr << "Can't create container, error = " << ErrorName(ret) << endl;

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
            cerr << "Can't destroy container, error = " << ErrorName(ret) << endl;

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
            cerr << "Can't list containers, error = " << ErrorName(ret) << endl;
        else
            for (auto c : clist) {
                string s;
                ret = api.GetData(c, "state", s);
                if (ret)
                    cerr << "Can't get container state, error = " << ErrorName(ret) << endl;
                cout << left << setw(40) << c
                     << setw(40) << s << endl;

            }

        return ret;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    TGetPropertyCmd() : ICmd("pget", 2, "<name> <property>", "get container property") {}

    int Execute(int argc, char *argv[])
    {
        string value;
        int ret = api.GetProperty(argv[0], argv[1], value);
        if (ret)
            cerr << "Can't get property, error = " << ErrorName(ret) << endl;
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
            cerr << "Can't set property, error = " << ErrorName(ret) << endl;

        return ret;
    }
};

class TGetDataCmd : public ICmd {
public:
    TGetDataCmd() : ICmd("dget", 2, "<name> <data>", "get container data") {}

    int Execute(int argc, char *argv[])
    {
        string value;
        int ret = api.GetData(argv[0], argv[1], value);
        if (ret)
            cerr << "Can't get data, error = " << ErrorName(ret) << endl;
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
            cerr << "Can't start container, error = " << ErrorName(ret) << endl;

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
            cerr << "Can't stop container, error = " << ErrorName(ret) << endl;

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
            cerr << "Can't pause container, error = " << ErrorName(ret) << endl;

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
            cerr << "Can't resume container, error = " << ErrorName(ret) << endl;

        return ret;
    }
};

extern int Selftest();
class TSelftestCmd : public ICmd {
public:
    TSelftestCmd() : ICmd("selftest", 0, "", "perform selftest") {}

    int Execute(int argc, char *argv[])
    {
        return Selftest();
    }
};

class TGetCmd : public ICmd {
public:
    TGetCmd() : ICmd("get", 1, "<name> <data>", "get container property or data") {}

    int Execute(int argc, char *argv[])
    {
        string value;
        int ret;

        if (argc <= 1) {
            vector<TProperty> plist;
            ret = api.Plist(plist);
            if (ret) {
                cerr << "Can't list properties, error = " << ErrorName(ret) << endl;
                return 1;
            }

            vector<TData> dlist;
            ret = api.Dlist(dlist);
            if (ret) {
                cerr << "Can't list data, error = " << ErrorName(ret) << endl;
                return 1;
            }

            for (auto p : plist) {
                ret = api.GetProperty(argv[0], p.name, value);
                if (!ret)
                    cout << p.name << " = " << value << endl;
            }

            for (auto d : dlist) {
                ret = api.GetData(argv[0], d.name, value);
                if (!ret)
                    cout << d.name << " = " << value << endl;
            }

            return 0;
        }

        ret = api.GetData(argv[0], argv[1], value);
        if (!ret) {
            cout << value << endl;
            return 0;
        }

        ret = api.GetProperty(argv[0], argv[1], value);
        if (!ret) {
            cout << value << endl;
            return 0;
        }

        cerr << "Invalid property or data = " << ErrorName(ret) << endl;

        return 1;
    }
};

void TryExec(int argc, char *argv[]) {
    string name(argv[1]);

    for (ICmd *cmd : commands)
        if (cmd->GetName() == name) {
            if (!cmd->ValidArgs(argc - 2, argv + 2)) {
                Usage(cmd->GetName().c_str());
                exit(EXIT_FAILURE);
            }

            exit(cmd->Execute(argc - 2, argv + 2));
        }
}

int main(int argc, char *argv[])
{
    commands = {
        new THelpCmd(),
        new TCreateCmd(),
        new TDestroyCmd(),
        new TListCmd(),
        new TStartCmd(),
        new TStopCmd(),
        new TPauseCmd(),
        new TResumeCmd(),
        new TGetPropertyCmd(),
        new TSetPropertyCmd(),
        new TGetDataCmd(),
        new TGetCmd(),
        new TRawCmd(),
        new TSelftestCmd()
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

    if (name == "-v" || name == "--version") {
        cout << GIT_REVISION <<endl;
        return EXIT_FAILURE;
    }

    try {
        // porto <command> <arg2> <arg2>
        TryExec(argc, argv);

        // porto <arg1> <command> <arg2>
        if (argc >= 2) {
            char *p = argv[1];
            argv[1] = argv[2];
            argv[2] = p;

            TryExec(argc, argv);
        }

        cerr << "Invalid command " << name << "!" << endl;
    } catch (string err) {
        cerr << err << endl;
    }

    return EXIT_FAILURE;
};
