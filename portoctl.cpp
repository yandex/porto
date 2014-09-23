#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <csignal>

#include "libporto.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <wordexp.h>
}

using namespace std;

static string DataValue(const string &name, const string &val) {
    if (name == "exit_status") {
        int status;
        if (StringToInt(val, status))
            return val;

        string ret;

        if (WIFEXITED(status))
            ret = "Container exited with " + to_string(WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            ret = "Container killed by signal " + to_string(WTERMSIG(status));
        else if (status == 0)
            ret = "Success";

        return ret;
    } else if (name == "errno") {
        int status;
        if (StringToInt(val, status))
            return val;

        string ret;

        if (status < 0)
            ret = "Prepare failed: " + string(strerror(-status));
        else if (status > 0)
            ret = "Exec failed: " + string(strerror(status));
        else if (status == 0)
            ret = "Success";

        return ret + " (" + val + ")";
    } else {
        return val;
    }
}

class ICmd {
protected:
    string Name, Usage, Desc;
    int NeedArgs;
    TPortoAPI Api;

public:
    ICmd(const string& name, int args, const string& usage, const string& desc) :
        Name(name), Usage(usage), Desc(desc), NeedArgs(args) {}

    string& GetName() { return Name; }
    string& GetUsage() { return Usage; }
    string& GetDescription() { return Desc; }

    const string &ErrorName(int err) {
        if (err == INT_MIN) {
            static const string err = "portod unavailable";
            return err;
        }
        return rpc::EError_Name(static_cast<rpc::EError>(err));
    }

    void PrintError(const TError &error, const string &str) {
        if (error.GetMsg().length())
            cerr << str << ": " << ErrorName(error.GetError()) << " (" << error.GetMsg() << ")" << endl;
        else
            cerr << str << ": " << ErrorName(error.GetError()) << endl;

    }

    void PrintError(const string &str) {
        int num;
        string msg;

        Api.GetLastError(num, msg);

        TError error((EError)num, msg);
        PrintError(error, str);
    }

    bool ValidArgs(int argc, char *argv[]) {
        if (argc < NeedArgs)
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
        const int nameWidth = 32;
        const int descWidth = 48;

        cout << "Usage: " << program_invocation_short_name << " <command> [<args>]" << endl;
        cout << endl;
        cout << "Command list:" << endl;
        for (ICmd *cmd : commands)
            cout << " " << left << setw(nameWidth) << cmd->GetName() << cmd->GetDescription() << endl;

        int ret;
        cout << endl << "Property list:" << endl;
        vector<TProperty> plist;
        ret = Api.Plist(plist);
        if (ret) {
            PrintError("Unavailable");
        } else
            for (auto p : plist)
                cout << " " << left << setw(nameWidth) << p.Name
                     << setw(descWidth) << p.Description << endl;

        cout << endl << "Data list:" << endl;
        vector<TData> dlist;
        ret = Api.Dlist(dlist);
        if (ret)
            PrintError("Unavailable");
        else
            for (auto d : dlist)
                cout << " " << left << setw(nameWidth) << d.Name
                     << setw(descWidth) << d.Description << endl;
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
                cout << "Usage: " << program_invocation_short_name << " " << name << " " << cmd->GetUsage() << endl;
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
        if (!Api.Raw(msg.str(), resp))
            cout << resp << endl;

        return 0;
    }
};

class TCreateCmd : public ICmd {
public:
    TCreateCmd() : ICmd("create", 1, "<name>", "create container") {}

    int Execute(int argc, char *argv[])
    {
        int ret = Api.Create(argv[0]);
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
        int ret = Api.Destroy(argv[0]);
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
        int ret = Api.List(clist);
        if (ret)
            PrintError("Can't list containers");
        else
            for (auto c : clist) {
                string s;
                ret = Api.GetData(c, "state", s);
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
        int ret = Api.GetProperty(argv[0], argv[1], value);
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

        int ret = Api.SetProperty(argv[0], argv[1], val);
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
        int ret = Api.GetData(argv[0], argv[1], value);
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
        int ret = Api.Start(argv[0]);
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
        int ret = Api.Stop(argv[0]);
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
        int ret = Api.Pause(argv[0]);
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
        int ret = Api.Resume(argv[0]);
        if (ret)
            PrintError("Can't resume container");

        return ret;
    }
};

class TGetCmd : public ICmd {
public:
    TGetCmd() : ICmd("get", 1, "<name> <data>", "get container property or data") {}

    bool ValidProperty(const vector<TProperty> &plist, const string &name) {
        return find_if(plist.begin(), plist.end(),
                       [&](const TProperty &i)->bool { return i.Name == name; })
            != plist.end();
    }

    bool ValidData(const vector<TData> &dlist, const string &name) {
        return find_if(dlist.begin(), dlist.end(),
                       [&](const TData &i)->bool { return i.Name == name; })
            != dlist.end();
    }


    int Execute(int argc, char *argv[])
    {
        string value;
        int ret;

        vector<TProperty> plist;
        ret = Api.Plist(plist);
        if (ret) {
            PrintError("Can't list properties");
            return 1;
        }

        vector<TData> dlist;
        ret = Api.Dlist(dlist);
        if (ret) {
            PrintError("Can't list data");
            return 1;
        }

        if (argc <= 1) {
            for (auto p : plist) {
                if (!ValidProperty(plist, p.Name))
                    continue;

                ret = Api.GetProperty(argv[0], p.Name, value);
                if (!ret)
                    cout << p.Name << " = " << value << endl;
            }

            for (auto d : dlist) {
                if (!ValidData(dlist, d.Name))
                    continue;

                ret = Api.GetData(argv[0], d.Name, value);
                if (!ret)
                    cout << d.Name << " = " << DataValue(d.Name, value) << endl;
            }

            return 0;
        }

        bool validProperty = ValidProperty(plist, argv[1]);
        bool validData = ValidData(dlist, argv[1]);

        if (validData) {
            ret = Api.GetData(argv[0], argv[1], value);
            if (!ret)
                cout << DataValue(argv[1], value) << endl;
            else if (ret != EError::InvalidData)
                PrintError("Can't get data");
        }

        if (validProperty) {
            ret = Api.GetProperty(argv[0], argv[1], value);
            if (!ret)
                cout << value << endl;
            else if (ret != EError::InvalidProperty)
                PrintError("Can't get data");
        }

        if (!validProperty && !validData)
            cerr << "Invalid property or data" << endl;

        return 1;
    }
};

class TEnterCmd : public ICmd {
public:
    TEnterCmd() : ICmd("enter", 1, "<name> [command]", "execute command in container namespace") {}

    void PrintErrno(const string &str) {
        cerr << str << ": " << strerror(errno) << endl;
    }

    int OpenFd(int pid, string v) {
        string path = "/proc/" + to_string(pid) + "/" + v;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            PrintErrno("Can't open [" + path + "] " + to_string(fd));
            throw "";
        }

        return fd;
    }

    int Execute(int argc, char *argv[])
    {
        string cmd = "";
        for (int i = 1; i < argc; i++) {
            cmd += argv[i];
            cmd += " ";
        }

        if (!cmd.length())
            cmd = "/bin/bash";

        // order is important
        pair<string, int> nameToType[] = {
            //{ "ns/user", CLONE_NEWUSER },
            { "ns/ipc", CLONE_NEWIPC },
            { "ns/uts", CLONE_NEWUTS },
            { "ns/net", CLONE_NEWNET },
            { "ns/pid", CLONE_NEWPID },
            { "ns/mnt", CLONE_NEWNS },
        };

        string pidStr;
        int ret = Api.GetData(argv[0], "root_pid", pidStr);
        if (ret) {
            PrintError("Can't get container root_pid");
            return EXIT_FAILURE;
        }

        int pid;
        TError error = StringToInt(pidStr, pid);
        if (error) {
            PrintError(error, "Can't parse root_pid");
            return EXIT_FAILURE;
        }

        int rootFd = OpenFd(pid, "root");
        int cwdFd = OpenFd(pid, "cwd");

        for (auto &p : nameToType) {
            int fd = OpenFd(pid, p.first);
            if (setns(fd, p.second)) {
                PrintErrno("Can't set namespace");
                return EXIT_FAILURE;
            }
            close(fd);
        }

        if (fchdir(rootFd) < 0) {
            PrintErrno("Can't change root directory");
            return EXIT_FAILURE;
        }

        if (chroot(".") < 0) {
            PrintErrno("Can't change root directory");
            return EXIT_FAILURE;
        }
        close(rootFd);

        if (fchdir(cwdFd) < 0) {
            PrintErrno("Can't change root directory");
            return EXIT_FAILURE;
        }
        close(cwdFd);

        wordexp_t result;
        ret = wordexp(cmd.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
        if (ret) {
            errno = EINVAL;
            PrintErrno("Can't parse command");
            return EXIT_FAILURE;
        }

        int status = EXIT_FAILURE;
        int child = fork();
        if (child) {
            if (waitpid(child, &status, 0) < 0)
                PrintErrno("Can't wait child");
        } else if (child < 0) {
            PrintErrno("Can't fork");
        } else {
            execvp(result.we_wordv[0], (char *const *)result.we_wordv);
            PrintErrno("Can't execute " + string(result.we_wordv[0]));
        }

        return status;
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
        new TEnterCmd(),
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

    // in case client closes pipe we are writing to in the protobuf code
    (void)RegisterSignal(SIGPIPE, SIG_IGN);

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
