#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <csignal>

#include "libporto.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "cli.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <wordexp.h>
}

using std::string;
using std::vector;
using std::stringstream;
using std::ostream_iterator;
using std::map;
using std::pair;
using std::set;
using std::shared_ptr;

string HumanTime(const string &val) {
    double n = stod(val);
    string suf = "ns";
    if (n > 1024) {
        n /= 1024;
        suf = "us";
    }
    if (n > 1024) {
        n /= 1024;
        suf = "ms";
    }
    if (n > 1024) {
        n /= 1024;
        suf = "s";
    }

    std::stringstream str;
    str << n << suf;
    return str.str();
}

string HumanSize(const string &val) {
    double n = stod(val);
    string suf = "";
    if (n > 1024) {
        n /= 1024;
        suf = "K";
    }
    if (n > 1024) {
        n /= 1024;
        suf = "M";
    }
    if (n > 1024) {
        n /= 1024;
        suf = "G";
    }

    std::stringstream str;
    str << n << suf;
    return str.str();
}

string PropertyValue(const string &name, const string &val) {
    if (name == "memory_guarantee" ||
        name == "memory_limit" ||
        name == "net_ceil" ||
        name == "net_guarantee") {
        return HumanSize(val);
    } else {
        return val;
    }
}

string DataValue(const string &name, const string &val) {
    if (name == "exit_status") {
        int status;
        if (StringToInt(val, status))
            return val;

        string ret;

        if (WIFEXITED(status))
            ret = "Container exited with " + std::to_string(WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            ret = "Container killed by signal " + std::to_string(WTERMSIG(status));
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
    } else if (name == "memory_usage" ||
               name == "net_drops" ||
               name == "net_overlimits" ||
               name == "net_packets" ||
               name == "net_bytes") {
        return HumanSize(val);
    } else if (name == "cpu_usage") {
        return HumanTime(val);
    } else {
        return val;
    }
}

size_t CalculateFieldLength(vector<string> &vec, size_t min = 8) {
    size_t len = 0;
    for (auto &i : vec)
        if (i.length() + 1 > len)
            len  = i.length() + 1;

    return len > min ? len : min;
}

class TRawCmd : public ICmd {
public:
    TRawCmd(TPortoAPI *api) : ICmd(api, "raw", 2, "<message>", "send raw protobuf message") {}

    int Execute(int argc, char *argv[]) {
        stringstream msg;

        std::vector<std::string> args(argv, argv + argc);
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

        string resp;
        if (!Api->Raw(msg.str(), resp))
            std::cout << resp << std::endl;

        return 0;
    }
};

class TCreateCmd : public ICmd {
public:
    TCreateCmd(TPortoAPI *api) : ICmd(api, "create", 1, "<name>", "create container") {}

    int Execute(int argc, char *argv[]) {
        int ret = Api->Create(argv[0]);
        if (ret)
            PrintError("Can't create container");

        return ret;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    TGetPropertyCmd(TPortoAPI *api) : ICmd(api, "pget", 2, "<name> <property>", "get container property") {}

    int Execute(int argc, char *argv[]) {
        string value;
        int ret = Api->GetProperty(argv[0], argv[1], value);
        if (ret)
            PrintError("Can't get property");
        else
            std::cout << value << std::endl;

        return ret;
    }
};

class TSetPropertyCmd : public ICmd {
public:
    TSetPropertyCmd(TPortoAPI *api) : ICmd(api, "set", 3, "<name> <property>", "set container property") {}

    int Execute(int argc, char *argv[]) {
        string val = argv[2];
        for (int i = 3; i < argc; i++) {
            val += " ";
            val += argv[i];
        }

        int ret = Api->SetProperty(argv[0], argv[1], val);
        if (ret)
            PrintError("Can't set property");

        return ret;
    }
};

class TGetDataCmd : public ICmd {
public:
    TGetDataCmd(TPortoAPI *api) : ICmd(api, "dget", 2, "<name> <data>", "get container data") {}

    int Execute(int argc, char *argv[]) {
        string value;
        int ret = Api->GetData(argv[0], argv[1], value);
        if (ret)
            PrintError("Can't get data");
        else
            std::cout << value << std::endl;

        return ret;
    }
};

class TStartCmd : public ICmd {
public:
    TStartCmd(TPortoAPI *api) : ICmd(api, "start", 1, "<name>", "start container") {}

    int Execute(int argc, char *argv[]) {
        int ret = Api->Start(argv[0]);
        if (ret)
            PrintError("Can't start container");

        return ret;
    }
};

static const map<string, int> sigMap = {
    { "SIGHUP",     SIGHUP },
    { "SIGINT",     SIGINT },
    { "SIGQUIT",    SIGQUIT },
    { "SIGILL",     SIGILL },
    { "SIGABRT",    SIGABRT },
    { "SIGFPE",     SIGFPE },
    { "SIGKILL",    SIGKILL },
    { "SIGSEGV",    SIGSEGV },
    { "SIGPIPE",    SIGPIPE },

    { "SIGALRM",    SIGALRM },
    { "SIGTERM",    SIGTERM },
    { "SIGUSR1",    SIGUSR1 },
    { "SIGUSR2",    SIGUSR2 },
    { "SIGCHLD",    SIGCHLD },
    { "SIGCONT",    SIGCONT },
    { "SIGSTOP",    SIGSTOP },
    { "SIGTSTP",    SIGTSTP },
    { "SIGTTIN",    SIGTTIN },
    { "SIGTTOU",    SIGTTOU },

    { "SIGBUS",     SIGBUS },
    { "SIGPOLL",    SIGPOLL },
    { "SIGPROF",    SIGPROF },
    { "SIGSYS",     SIGSYS },
    { "SIGTRAP",    SIGTRAP },
    { "SIGURG",     SIGURG },
    { "SIGVTALRM",  SIGVTALRM },
    { "SIGXCPU",    SIGXCPU },
    { "SIGXFSZ",    SIGXFSZ },

    { "SIGIOT",     SIGIOT },
#ifdef SIGEMT
    { "SIGEMT",     SIGEMT },
#endif
    { "SIGSTKFLT",  SIGSTKFLT },
    { "SIGIO",      SIGIO },
    { "SIGCLD",     SIGCLD },
    { "SIGPWR",     SIGPWR },
#ifdef SIGINFO
    { "SIGINFO",    SIGINFO },
#endif
#ifdef SIGLOST
    { "SIGLOST",    SIGLOST },
#endif
    { "SIGWINCH",   SIGWINCH },
    { "SIGUNUSED",  SIGUNUSED },
};

class TKillCmd : public ICmd {
public:
    TKillCmd(TPortoAPI *api) : ICmd(api, "kill", 1, "<name> [signal]", "send signal to container") {}

    int Execute(int argc, char *argv[]) {
        int sig = SIGTERM;
        if (argc >= 2) {
            string sigName = argv[1];

            if (sigMap.find(sigName) != sigMap.end()) {
                sig = sigMap.at(sigName);
            } else {
                TError error = StringToInt(sigName, sig);
                if (error) {
                    PrintError(error, "Invalid signal");
                    return EXIT_FAILURE;
                }
            }
        }

        int ret = Api->Kill(argv[0], sig);
        if (ret)
            PrintError("Can't send signal to container");

        return ret;
    }
};

class TStopCmd : public ICmd {
public:
    TStopCmd(TPortoAPI *api) : ICmd(api, "stop", 1, "<name>", "stop container") {}

    int Execute(int argc, char *argv[]) {
        int ret = Api->Stop(argv[0]);
        if (ret)
            PrintError("Can't stop container");

        return ret;
    }
};

class TPauseCmd : public ICmd {
public:
    TPauseCmd(TPortoAPI *api) : ICmd(api, "pause", 1, "<name>", "pause container") {}

    int Execute(int argc, char *argv[]) {
        int ret = Api->Pause(argv[0]);
        if (ret)
            PrintError("Can't pause container");

        return ret;
    }
};

class TResumeCmd : public ICmd {
public:
    TResumeCmd(TPortoAPI *api) : ICmd(api, "resume", 1, "<name>", "resume container") {}

    int Execute(int argc, char *argv[]) {
        int ret = Api->Resume(argv[0]);
        if (ret)
            PrintError("Can't resume container");

        return ret;
    }
};

class TGetCmd : public ICmd {
public:
    TGetCmd(TPortoAPI *api) : ICmd(api, "get", 1, "<name> [data]", "get container property or data") {}

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

    int Execute(int argc, char *argv[]) {
        string value;
        int ret;

        vector<TProperty> plist;
        ret = Api->Plist(plist);
        if (ret) {
            PrintError("Can't list properties");
            return EXIT_FAILURE;
        }

        vector<TData> dlist;
        ret = Api->Dlist(dlist);
        if (ret) {
            PrintError("Can't list data");
            return EXIT_FAILURE;
        }

        if (argc <= 1) {
            int printed = 0;

            for (auto p : plist) {
                if (!ValidProperty(plist, p.Name))
                    continue;

                ret = Api->GetProperty(argv[0], p.Name, value);
                if (!ret) {
                    std::cout << p.Name << " = " << PropertyValue(p.Name, value) << std::endl;
                    printed++;
                }
            }

            for (auto d : dlist) {
                if (!ValidData(dlist, d.Name))
                    continue;

                ret = Api->GetData(argv[0], d.Name, value);
                if (!ret) {
                    std::cout << d.Name << " = " << DataValue(d.Name, value) << std::endl;
                    printed++;
                }
            }

            if (!printed)
                    std::cerr << "Invalid container name" << std::endl;

            return 0;
        }

        bool validProperty = ValidProperty(plist, argv[1]);
        bool validData = ValidData(dlist, argv[1]);

        if (validData) {
            ret = Api->GetData(argv[0], argv[1], value);
            if (!ret)
                std::cout << DataValue(argv[1], value) << std::endl;
            else if (ret != EError::InvalidData)
                PrintError("Can't get data");
        }

        if (validProperty) {
            ret = Api->GetProperty(argv[0], argv[1], value);
            if (!ret) {
                std::cout << PropertyValue(argv[1], value) << std::endl;
            } else if (ret != EError::InvalidProperty) {
                PrintError("Can't get data");
                return EXIT_FAILURE;
            }
        }

        if (!validProperty && !validData) {
            std::cerr << "Invalid property or data" << std::endl;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }
};

class TEnterCmd : public ICmd {
public:
    TEnterCmd(TPortoAPI *api) : ICmd(api, "enter", 1, "<name> [-C] [command]", "execute command in container namespace") {}

    void PrintErrno(const string &str) {
        std::cerr << str << ": " << strerror(errno) << std::endl;
    }

    int OpenFd(int pid, string v) {
        string path = "/proc/" + std::to_string(pid) + "/" + v;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            PrintErrno("Can't open [" + path + "] " + std::to_string(fd));
            throw "";
        }

        return fd;
    }

    TError GetCgMount(const string &subsys, string &root) {
        vector<string> subsystems;
        TError error = SplitString(subsys, ',', subsystems);
        if (error)
            return error;

        TMountSnapshot ms;
        set<shared_ptr<TMount>> mounts;
        error = ms.Mounts(mounts);
        if (error)
            return error;

        for (auto &mount : mounts) {
            set<string> flags = mount->GetFlags();
            bool found = true;
            for (auto &ss : subsystems)
                if (flags.find(ss) == flags.end()) {
                    found = false;
                    break;
                }

            if (found) {
                root = mount->GetMountpoint();
                return TError::Success();
            }
        }

        return TError(EError::Unknown, "Can't find root for " + subsys);
    }

    int Execute(int argc, char *argv[]) {
        string cmd = "";
        int start = 1;
        bool enterCgroups = true;

        if (argc >= 2) {
            if (argv[1] == string("-C"))
                enterCgroups = false;

            start++;
        }

        for (int i = start; i < argc; i++) {
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
        int ret = Api->GetData(argv[0], "root_pid", pidStr);
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

        if (enterCgroups) {
            map<string, string> cgmap;
            TError error = GetTaskCgroups(pid, cgmap);
            if (error) {
                PrintError(error, "Can't get task cgroups");
                return EXIT_FAILURE;

            }

            for (auto &cg : cgmap) {
                string root;
                TError error = GetCgMount(cg.first, root);
                if (error) {
                    PrintError(error, "Can't get task cgroups");
                    return EXIT_FAILURE;
                }

                TFile f(root + cg.second + "/cgroup.procs");
                error = f.AppendString(std::to_string(GetPid()));
                if (error) {
                    PrintError(error, "Can't get task cgroups");
                    return EXIT_FAILURE;
                }
            }
        }

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

class TRunCmd : public ICmd {
public:
    TRunCmd(TPortoAPI *api) : ICmd(api, "run", 2, "<container> [properties]", "create and start container with given properties") {}

    int Parser(string property, map<string, string> &properties) {
        string propertyKey, propertyValue;
        string::size_type n;
        n = property.find('=');
        if (n == string::npos) {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property: " + property);
            return EXIT_FAILURE;
        }
        propertyKey = property.substr(0, n);
        propertyValue = property.substr(n + 1, property.size());
        if (propertyKey == "" || propertyValue == "") {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property: " + property);
            return EXIT_FAILURE;
        }
        properties[propertyKey] = propertyValue;
        return EXIT_SUCCESS;
    }

    int Execute(int argc, char *argv[]) {
        string containerName = argv[0];
        map<string, string> properties;
        int ret;

        for (int i = 1; i < argc; i++) {
            ret = Parser(argv[i], properties);
            if (ret)
                return ret;
        }

        ret = Api->Create(containerName);
        if (ret) {
            PrintError("Can't create container");
            return EXIT_FAILURE;
        }
        for (auto iter: properties) {
            ret = Api->SetProperty(containerName, iter.first, iter.second);
            if (ret) {
                PrintError("Can't set property");
                (void)Api->Destroy(containerName);
                return EXIT_FAILURE;
            }
        }
        ret = Api->Start(containerName);
        if (ret) {
            PrintError("Can't start property");
            (void)Api->Destroy(containerName);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
};

class TDestroyCmd : public ICmd {
public:
    TDestroyCmd(TPortoAPI *api) : ICmd(api, "destroy", 1, "<name>", "destroy container") {}

    int Execute(int argc, char *argv[]) {
        int ret = Api->Destroy(argv[0]);
        if (ret)
            PrintError("Can't destroy container");

        return ret;
    }
};

class TListCmd : public ICmd {
public:
    TListCmd(TPortoAPI *api) : ICmd(api, "list", 0, "", "list created containers") {}

    int Execute(int argc, char *argv[]) {
        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        size_t nameLen = CalculateFieldLength(clist);
        for (auto c : clist) {
            string s;
            ret = Api->GetData(c, "state", s);
            if (ret)
                PrintError("Can't get container state");
            std::cout << std::left << std::setw(nameLen) << c << s << std::endl;
        }

        return EXIT_SUCCESS;
    }
};

class TTopCmd : public ICmd {
public:
    TTopCmd(TPortoAPI *api) : ICmd(api, "top", 0, "[sort-by]", "print containers sorted by resource usage") {}

    int GetUintData(const std::string &container,
                    const std::string &data, int64_t &val) {
        string s;
        int ret = Api->GetData(container, data, s);
        if (ret) {
            PrintError("Can't get container data " + data);
            return EXIT_FAILURE;
        }

        TError error = StringToInt64(s, val);
        if (error) {
            PrintError("Can't parse container data " + data);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    int Execute(int argc, char *argv[]) {
        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return EXIT_FAILURE;
        }

        vector<pair<string, map<string, int64_t>>> containerData;
        vector<string> showData = {
            "cpu_usage",
            "memory_usage",
            "major_faults",
        };

        if (config().network().enabled()) {
            showData.push_back("net_packets");
            showData.push_back("net_drops");
            showData.push_back("net_overlimits");
        }

        size_t nameLen = CalculateFieldLength(clist, strlen("container"));
        size_t dataLen = CalculateFieldLength(showData);

        string sortBy = "cpu_usage";
        if (argc >= 1) {
            string arg = argv[0];

            if (std::find(showData.begin(), showData.end(), arg)
                == showData.end()) {
                TError error(EError::InvalidValue, "Invalid value");

                PrintError(error, "Can't parse argument");
                return EXIT_FAILURE;
            }

            sortBy = arg;
        }

        for (auto container : clist) {
            string state;
            ret = Api->GetData(container, "state", state);
            if (ret) {
                PrintError("Can't get container state");
                return EXIT_FAILURE;
            }

            if (state != "running")
                continue;

            map<string, int64_t> dataVal;
            for (auto data : showData) {
                int64_t val;
                if (GetUintData(container, data, val))
                    return EXIT_FAILURE;

                dataVal[data] = val;
            }

            containerData.push_back(make_pair(container, dataVal));
        }

        std::sort(containerData.begin(), containerData.end(),
                  [&](pair<string, map<string, int64_t>> a,
                     pair<string, map<string, int64_t>> b) {
                  return a.second[sortBy] > b.second[sortBy];
                  });

        std::cout << std::left << std::setw(nameLen) << "container";
        for (auto &data : showData)
            std::cout << std::right << std::setw(dataLen) << data;
        std::cout << std::endl;

        for (auto &pair : containerData) {
            std::cout << std::left << std::setw(nameLen) << pair.first;

            for (auto &data : showData)
                std::cout << std::right << std::setw(dataLen) <<
                    DataValue(data, std::to_string(pair.second[data]));
            std::cout << std::endl;
        }

        return ret;
    }
};

int main(int argc, char *argv[]) {
    config.Load(true);
    TPortoAPI api(config().rpc_sock().file().path());

    RegisterCommand(new THelpCmd(&api, true));
    RegisterCommand(new TCreateCmd(&api));
    RegisterCommand(new TDestroyCmd(&api));
    RegisterCommand(new TListCmd(&api));
    RegisterCommand(new TTopCmd(&api));
    RegisterCommand(new TStartCmd(&api));
    RegisterCommand(new TStopCmd(&api));
    RegisterCommand(new TKillCmd(&api));
    RegisterCommand(new TPauseCmd(&api));
    RegisterCommand(new TResumeCmd(&api));
    RegisterCommand(new TGetPropertyCmd(&api));
    RegisterCommand(new TSetPropertyCmd(&api));
    RegisterCommand(new TGetDataCmd(&api));
    RegisterCommand(new TGetCmd(&api));
    RegisterCommand(new TRawCmd(&api));
    RegisterCommand(new TEnterCmd(&api));
    RegisterCommand(new TRunCmd(&api));

    return HandleCommand(argc, argv);
};
