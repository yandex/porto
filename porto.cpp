#include <iostream>
#include <iomanip>
#include <sstream>

#include "libporto.hpp"
#include "util/string.hpp"

using namespace std;

static string DataValue(const string &name, const string &val) {
    if (name == "exit_status") {
        int status;
        if (StringToInt(val, status))
            return val;

        string ret = val + " ";

        if (WIFEXITED(status))
            ret += "Container exited with " + to_string(status);
        else if (WIFSIGNALED(status))
            ret += "Container killed by signal " + to_string(status);
        else if (status == 0)
            ret += "Success";

        return ret;
    } else if (name == "errno") {
        int status;
        if (StringToInt(val, status))
            return val;

        string ret = val + " ";

        if (status < 0)
            ret += "Prepare failed: " + string(strerror(-status));
        else if (status > 0)
            ret += "Exec failed: " + string(strerror(status));
        else if (status == 0)
            ret += "Success";

        return ret;
    } else {
        return val;
    }
}

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
        if (err == INT_MIN) {
            static const string err = "portod unavailable";
            return err;
        }
        return rpc::EError_Name(static_cast<rpc::EError>(err));
    }

    void PrintError(const string &str) {
        int error;
        string msg;
        api.GetLastError(error, msg);
        if (msg.length())
            cerr << str << ": " << msg << " (" << ErrorName(error) << ")" << endl;
        else
            cerr << str << ": " << ErrorName(error) << endl;
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
            cout << " " << left << setw(24) << cmd->GetName() << cmd->GetDescription() << endl;

        int ret;
        cout << endl << "list of properties:" << endl;
        vector<TProperty> plist;
        ret = api.Plist(plist);
        if (ret)
            PrintError("Can't list properties");
        else
            for (auto p : plist)
                cout << " " << left << setw(24) << p.name
                     << setw(40) << p.description << endl;

        cout << endl << "list of data:" << endl;
        vector<TData> dlist;
        ret = api.Dlist(dlist);
        if (ret)
            PrintError("Can't list data");
        else
            for (auto d : dlist)
                cout << " " << left << setw(24) << d.name
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
            PrintError("Can't create container");

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
            PrintError("Can't destroy container");

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
            PrintError("Can't list containers");
        else
            for (auto c : clist) {
                string s;
                ret = api.GetData(c, "state", s);
                if (ret)
                    PrintError("Can't get container state");
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
            PrintError("Can't get property");
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
        string val = argv[2];
        for (int i = 3; i < argc; i++) {
            val += " ";
            val += argv[i];
        }

        int ret = api.SetProperty(argv[0], argv[1], val);
        if (ret)
            PrintError("Can't set property");

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
            PrintError("Can't get data");
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
            PrintError("Can't start container");

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
            PrintError("Can't stop container");

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
            PrintError("Can't pause container");

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
            PrintError("Can't resume container");

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

extern int StressTest();
class TStressTestCmd : public ICmd {
public:
    TStressTestCmd() : ICmd("stresstest", 0, "", "perform stresstest") {}

    int Execute(int argc, char *argv[])
    {
        return StressTest();
    }
};

class TGetCmd : public ICmd {
public:
    TGetCmd() : ICmd("get", 1, "<name> <data>", "get container property or data") {}

    int Execute(int argc, char *argv[])
    {
        string value;
        int ret;

        vector<TProperty> plist;
        ret = api.Plist(plist);
        if (ret) {
            PrintError("Can't list properties");
            return 1;
        }

        vector<TData> dlist;
        ret = api.Dlist(dlist);
        if (ret) {
            PrintError("Can't list data");
            return 1;
        }

        if (argc <= 1) {
            for (auto p : plist) {
                ret = api.GetProperty(argv[0], p.name, value);
                if (!ret)
                    cout << p.name << " = " << value << endl;
            }

            for (auto d : dlist) {
                ret = api.GetData(argv[0], d.name, value);
                if (!ret)
                    cout << d.name << " = " << DataValue(d.name, value) << endl;
            }

            return 0;
        }

        bool validProperty = false;
        for (auto p : plist)
            if (p.name == argv[1]) {
                validProperty = true;
                break;
            }

        bool validData = false;
        for (auto d : dlist)
            if (d.name == argv[1]) {
                validData = true;
                break;
            }

        if (validData) {
            ret = api.GetData(argv[0], argv[1], value);
            if (!ret)
                cout << DataValue(argv[1], value) << endl;
        }

        if (validProperty) {
            ret = api.GetProperty(argv[0], argv[1], value);
            if (!ret)
                cout << value << endl;
        }

        if (!validProperty && !validData)
            PrintError("Invalid property or data");

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
        new TSelftestCmd(),
        new TStressTestCmd(),
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
        cout << GIT_TAG << " " << GIT_REVISION <<endl;
        return EXIT_FAILURE;
    }

    try {
        // porto <command> <arg2> <arg2>
        TryExec(argc, argv);

#if 0
        // porto <arg1> <command> <arg2>
        if (argc >= 2) {
            char *p = argv[1];
            argv[1] = argv[2];
            argv[2] = p;

            TryExec(argc, argv);
        }
#endif

        cerr << "Invalid command " << name << "!" << endl;
    } catch (string err) {
        cerr << err << endl;
    }

    return EXIT_FAILURE;
};
