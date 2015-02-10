#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <csignal>

#include "libporto.hpp"
#include "config.hpp"
#include "cli.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/namespace.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <wordexp.h>
#include <termios.h>
#include <poll.h>
}

using std::string;
using std::vector;
using std::stringstream;
using std::ostream_iterator;
using std::map;
using std::pair;
using std::set;
using std::shared_ptr;

string HumanNsec(const string &val) {
    double n = stod(val);
    string suf = "ns";
    if (n > 1000) {
        n /= 1000;
        suf = "us";
    }
    if (n > 1000) {
        n /= 1000;
        suf = "ms";
    }
    if (n > 1000) {
        n /= 1000;
        suf = "s";
    }

    std::stringstream str;
    str << n << suf;
    return str.str();
}

string HumanSec(const string &val) {
    int64_t n = stoll(val);
    int64_t h = 0, m = 0, s = n;

    if (s > 60) {
        m = s / 60;
        s %= 60;
    }

    if (m > 60) {
        h = m / 60;
        m %= 60;
    }

    std::stringstream str;
    if (h)
        str << std::setfill('0') << std::setw(2) << h << ":";
    str << std::setfill('0') << std::setw(2) << m << ":";
    str << std::setfill('0') << std::setw(2) << s;
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
        name == "memory_limit") {
        return HumanSize(val);
    } else {
        return val;
    }
}

string DataValue(const string &name, const string &val) {
    if (val == "")
        return val;

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
    } else if (name == "memory_usage") {
        return HumanSize(val);
    } else if (name == "cpu_usage") {
        return HumanNsec(val);
    } else if (name == "time") {
        return HumanSec(val);
    } else {
        return val;
    }
}

size_t CalculateFieldLength(vector<string> &vec, size_t min = 8) {
    size_t len = 0;
    for (auto &i : vec)
        if (i.length() > len)
            len  = i.length();

    return (len > min ? len : min) + 1;
}

const std::string StripIdx(const std::string &name) {
    if (name.find('[') != std::string::npos)
        return std::string(name.c_str(), name.find('['));
    else
        return name;
}

bool ValidData(const vector<TData> &dlist, const string &name) {
    return find_if(dlist.begin(), dlist.end(),
                   [&](const TData &i)->bool { return i.Name == StripIdx(name); })
        != dlist.end();
}

bool ValidProperty(const vector<TProperty> &plist, const string &name) {
    return find_if(plist.begin(), plist.end(),
                   [&](const TProperty &i)->bool { return i.Name == StripIdx(name); })
        != plist.end();
}

string HumanValue(const string &name, const string &val) {
    return DataValue(name, PropertyValue(name, val));
}

class TRawCmd : public ICmd {
public:
    TRawCmd(TPortoAPI *api) : ICmd(api, "raw", 1, "<message>", "send raw protobuf message") {}

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
    TCreateCmd(TPortoAPI *api) : ICmd(api, "create", 1, "<name> [name...]", "create container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Create(argv[i]);
            if (ret) {
                PrintError("Can't create container");
                return ret;
            }
        }

        return 0;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    TGetPropertyCmd(TPortoAPI *api) : ICmd(api, "pget", 2, "<name> <property> [property...]", "get raw container property") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 1; i < argc; i++) {
            string value;
            int ret = Api->GetProperty(argv[0], argv[i], value);
            if (ret) {
                PrintError("Can't get property");
                return ret;
            }
            Print(value);
        }

        return 0;
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
    TGetDataCmd(TPortoAPI *api) : ICmd(api, "dget", 2, "<name> <data> [data...]", "get raw container data") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 1; i < argc; i++) {
            string value;
            int ret = Api->GetData(argv[0], argv[i], value);
            if (ret) {
                PrintError("Can't get data");
                return ret;
            }
            Print(value);
        }

        return 0;
    }
};

class TStartCmd : public ICmd {
public:
    TStartCmd(TPortoAPI *api) : ICmd(api, "start", 1, "<name> [name...]", "start container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Start(argv[i]);
            if (ret) {
                PrintError("Can't start container");
                return ret;
            }
        }

        return 0;
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
    TStopCmd(TPortoAPI *api) : ICmd(api, "stop", 1, "<name> [name...]", "stop container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Stop(argv[0]);
            if (ret) {
                PrintError("Can't stop container");
                return ret;
            }
        }

        return 0;
    }
};

class TRestartCmd : public ICmd {
public:
    TRestartCmd(TPortoAPI *api) : ICmd(api, "restart", 1, "<name> [name...]", "restart container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Stop(argv[0]);
            if (ret) {
                PrintError("Can't stop container");
                return ret;
            }

            ret = Api->Start(argv[0]);
            if (ret) {
                PrintError("Can't start container");
                return ret;
            }
        }

        return 0;
    }
};

class TPauseCmd : public ICmd {
public:
    TPauseCmd(TPortoAPI *api) : ICmd(api, "pause", 1, "<name> [name...]", "pause container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Pause(argv[0]);
            if (ret) {
                PrintError("Can't pause container");
                return ret;
            }
        }

        return 0;
    }
};

class TResumeCmd : public ICmd {
public:
    TResumeCmd(TPortoAPI *api) : ICmd(api, "resume", 1, "<name> [name...]", "resume container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Resume(argv[0]);
            if (ret) {
                PrintError("Can't resume container");
                return ret;
            }
        }

        return 0;
    }
};

class TGetCmd : public ICmd {
public:
    TGetCmd(TPortoAPI *api) : ICmd(api, "get", 1, "<name> <variable> [variable...]", "get container property or data") {}

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
                    Print(p.Name + " = " + PropertyValue(p.Name, value));
                    printed++;
                }
            }

            for (auto d : dlist) {
                if (!ValidData(dlist, d.Name))
                    continue;

                ret = Api->GetData(argv[0], d.Name, value);
                if (!ret) {
                    Print(d.Name + " = " + DataValue(d.Name, value));
                    printed++;
                }
            }

            if (!printed)
                    std::cerr << "Invalid container name" << std::endl;

            return 0;
        }

        for (int i = 1; i < argc; i++) {
            bool validProperty = ValidProperty(plist, argv[i]);
            bool validData = ValidData(dlist, argv[i]);

            if (validData) {
                ret = Api->GetData(argv[0], argv[i], value);
                if (!ret)
                    Print(DataValue(argv[i], value));
                else if (ret != EError::InvalidData)
                    PrintError("Can't get data");
            }

            if (validProperty) {
                ret = Api->GetProperty(argv[0], argv[i], value);
                if (!ret) {
                    Print(PropertyValue(argv[i], value));
                } else if (ret != EError::InvalidProperty) {
                    PrintError("Can't get data");
                    return EXIT_FAILURE;
                }
            }

            if (!validProperty && !validData) {
                std::cerr << "Invalid property or data" << std::endl;
                return EXIT_FAILURE;
            }
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
            set<string> data = mount->GetData();
            bool found = true;
            for (auto &ss : subsystems)
                if (data.find(ss) == data.end()) {
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

        TNamespaceSnapshot ns;
        error = ns.Create(pid);
        if (error) {
            PrintError(error, "Can't create namespace snapshot");
            return EXIT_FAILURE;
        }

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

        error = ns.Attach();
        if (error) {
            PrintError(error, "Can't create namespace snapshot");
            return EXIT_FAILURE;
        }

        error = ns.Chroot();
        if (error) {
            PrintError(error, "Can't change root directory");
            return EXIT_FAILURE;
        }

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
            PrintError(error, "Can't parse property (no value): " + property);
            return EXIT_FAILURE;
        }
        propertyKey = property.substr(0, n);
        propertyValue = property.substr(n + 1, property.size());
        if (propertyKey == "" || propertyValue == "") {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property (key or value is nil): " + property);
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
            PrintError("Can't start container");
            (void)Api->Destroy(containerName);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
};

static struct termios savedAttrs;
static string destroyContainerName;
static void resetInputMode(void) {
  tcsetattr (STDIN_FILENO, TCSANOW, &savedAttrs);
}

static void destroyContainer(void) {
    if (destroyContainerName != "") {
        TPortoAPI api(config().rpc_sock().file().path());
        (void)api.Destroy(destroyContainerName);
    }
}

static void removeTempDir(int status, void *p) {
    TFolder f((char *)p);
    (void)f.Remove(true);
}

class TExecCmd : public ICmd {
    string containerName;
    sig_atomic_t Interrupted = 0;
public:
    TExecCmd(TPortoAPI *api) : ICmd(api, "exec", 2, "<container> [properties]", "create pty, execute and wait for command in container") {}

    int SwithToNonCanonical(int fd) {
        if (!isatty(fd))
            return 0;

        if (tcgetattr(fd, &savedAttrs) < 0)
            return -1;
        atexit(resetInputMode);

        static struct termios t = savedAttrs;
        t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
        t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
                       INPCK | ISTRIP | IXON | PARMRK);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        return tcsetattr(fd, TCSAFLUSH, &t);
    }

    void MoveData(int from, int to) {
        char buf[256];
        int ret;

        ret = read(from, buf, sizeof(buf));
        if (ret > 0)
            if (write(to, buf, ret) != ret)
                std::cerr << "Partial write to " << to << std::endl;
    }

    int MakeFifo(const std::string &path) {
        if (mkfifo(path.c_str(), 0755) < 0) {
            TError error(EError::Unknown, errno, "mkfifo()");
            PrintError(error, "Can't create temporary file " + path);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    int OpenTemp(const std::string &path, int flags) {
        int fd = open(path.c_str(), flags);
        if (fd < 0) {
            TError error(EError::Unknown, errno, "open()");
            PrintError(error, "Can't open temporary file " + path);
        }

        return fd;
    }

    int Execute(int argc, char *argv[]) {
        containerName = argv[0];
        bool hasTty = isatty(STDIN_FILENO);
        vector<const char *> args;
        std::string env;

        if (hasTty)
            env = std::string("TERM=") + getenv("TERM");

        for (int i = 0; i < argc; i++)
            if (strncmp(argv[i], "env=", 4) == 0)
                env = env + "; " + std::string(argv[i] + 4);
            else
                args.push_back(argv[i]);

        vector<struct pollfd> fds;

        struct pollfd pfd = {};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        fds.push_back(pfd);

        string stdinPath, stdoutPath, stderrPath;
        int stdinFd, stdoutFd, stderrFd;

        if (hasTty) {
            int ptm = posix_openpt(O_RDWR);
            if (ptm < 0) {
                TError error(EError::Unknown, errno, "posix_openpt()");
                PrintError(error, "Can't open pseudoterminal");
                return EXIT_FAILURE;
            }

            struct winsize ws;
            if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
                (void)ioctl(ptm, TIOCSWINSZ, &ws);

            if (grantpt(ptm) < 0) {
                TError error(EError::Unknown, errno, "grantpt()");
                PrintError(error, "Can't open pseudoterminal");
                return EXIT_FAILURE;
            }

            if (unlockpt(ptm) < 0) {
                TError error(EError::Unknown, errno, "unlockpt()");
                PrintError(error, "Can't open pseudoterminal");
                return EXIT_FAILURE;
            }

            char *slavept = ptsname(ptm);

            if (SwithToNonCanonical(STDIN_FILENO) < 0) {
                TError error(EError::Unknown, errno, "SwithToNonCanonical()");
                PrintError(error, "Can't open pseudoterminal");
                return EXIT_FAILURE;
            }

            pfd.fd = ptm;
            pfd.events = POLLIN;
            fds.push_back(pfd);

            stdinPath = slavept;
            stdoutPath = slavept;
            stderrPath = slavept;

            stdinFd = ptm;
            stdoutFd = ptm;
            stderrFd = ptm;
        } else {
            char *dir = mkdtemp(strdup("/tmp/portoctl-XXXXXX"));
            if (!dir) {
                TError error(EError::Unknown, errno, "mkdtemp()");
                PrintError(error, "Can't create temporary directory");
                return EXIT_FAILURE;
            }

            on_exit(removeTempDir, dir);

            stdinPath = string(dir) + "/stdin";
            if (MakeFifo(stdinPath))
                return EXIT_FAILURE;

            stdoutPath = string(dir) + "/stdout";
            if (MakeFifo(stdoutPath))
                return EXIT_FAILURE;

            stderrPath = string(dir) + "/stderr";
            if (MakeFifo(stderrPath))
                return EXIT_FAILURE;

            stdinFd = OpenTemp(stdinPath, O_RDWR | O_NONBLOCK);
            if (stdinFd < 0)
                return EXIT_FAILURE;

            stdoutFd = OpenTemp(stdoutPath, O_RDONLY | O_NONBLOCK);
            if (stdoutFd < 0)
                return EXIT_FAILURE;

            stderrFd = OpenTemp(stderrPath, O_RDONLY | O_NONBLOCK);
            if (stderrFd < 0)
                return EXIT_FAILURE;

            pfd.fd = stdoutFd;
            pfd.events = POLLIN;
            fds.push_back(pfd);

            pfd.fd = stderrFd;
            pfd.events = POLLIN;
            fds.push_back(pfd);
        }

        args.push_back(strdup(("stdin_path=" + stdinPath).c_str()));
        args.push_back(strdup(("stdout_path=" + stdoutPath).c_str()));
        args.push_back(strdup(("stderr_path=" + stderrPath).c_str()));
        if (env.length()) {
            env = "env=" + env;
            args.push_back(env.c_str());
        }

        auto *run = new TRunCmd(Api);
        int ret = run->Execute(args.size(), (char **)args.data());
        if (ret)
            return ret;

        destroyContainerName = containerName;
        atexit(destroyContainer);

        bool hangup = false;
        while (!hangup) {
            ret = poll(fds.data(), fds.size(), -1);
            if (ret < 0)
                break;

            for (size_t i = 0; i < fds.size(); i++) {
                if (fds[i].fd != STDIN_FILENO && fds[i].revents & POLLHUP)
                    hangup = true;

                if (!(fds[i].revents & POLLIN))
                    continue;

                if (i == 0)
                    MoveData(STDIN_FILENO, stdinFd);
                else if (i == 1)
                    MoveData(stdoutFd, STDOUT_FILENO);
                else if (i == 2)
                    MoveData(stderrFd, STDERR_FILENO);
            }
        }

        string state;
        int loop = 1000;
        do {
            usleep(1000 * 1000);
            ret = Api->GetData(containerName, "state", state);
            if (ret) {
                PrintError("Can't get state");
                return EXIT_FAILURE;
            }

            if (Interrupted) {
                ResetAllSignalHandlers();
                raise(InterruptedSignal);
                exit(EXIT_FAILURE);
            }
        } while (loop-- && state == "running");

        string s;
        ret = Api->GetData(containerName, "exit_status", s);
        if (ret) {
            PrintError("Can't get exit_status");
            return EXIT_FAILURE;
        }

        int status = stoi(s);
        if (WIFEXITED(status)) {
            exit(WEXITSTATUS(status));
        } else {
            ResetAllSignalHandlers();
            raise(WTERMSIG(status));
            exit(EXIT_FAILURE);
        }

        return EXIT_SUCCESS;
    }
};

class TDestroyCmd : public ICmd {
public:
    TDestroyCmd(TPortoAPI *api) : ICmd(api, "destroy", 1, "<name> [name...]", "destroy container") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->Destroy(argv[i]);
            if (ret) {
                PrintError("Can't destroy container");
                return ret;
            }
        }

        return 0;
    }
};

class TListCmd : public ICmd {
public:
    TListCmd(TPortoAPI *api) : ICmd(api, "list", 0, "[-1]", "list created containers") {}

    int Execute(int argc, char *argv[]) {
        bool details = true;
        if (argc >= 1 && argv[0] == std::string("-1"))
            details = false;

        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        vector<string> states = { "running", "dead", "stopped", "paused" };
        size_t stateLen = CalculateFieldLength(states);
        size_t nameLen = CalculateFieldLength(clist);
        size_t timeLen = 10;
        for (auto c : clist) {
            if (c == "/")
                continue;

            std::cout << std::left << std::setw(nameLen) << c;

            if (details) {
                string s;
                ret = Api->GetData(c, "state", s);
                if (ret)
                    PrintError("Can't get container state");

                std::cout << std::right << std::setw(stateLen) << s;

                if (s == "running" || s == "dead") {
                    string tm;
                    ret = Api->GetData(c, "time", tm);
                    if (!ret)
                            std::cout << std::right << std::setw(timeLen)
                                << DataValue("time", tm);
                }
            }

            std::cout << std::endl;
        }

        return EXIT_SUCCESS;
    }
};

class TTopCmd : public ICmd {
public:
    TTopCmd(TPortoAPI *api) : ICmd(api, "top", 0, "[sort-by]", "print containers sorted by resource usage") {}

    int Execute(int argc, char *argv[]) {
        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return EXIT_FAILURE;
        }

        vector<pair<string, map<string, string>>> containerData;
        vector<string> showData;

        if (argc == 0) {
            showData.push_back("cpu_usage");
            showData.push_back("memory_usage");
            showData.push_back("major_faults");
            showData.push_back("minor_faults");

            if (config().network().enabled())
                showData.push_back("net_packets");

            showData.push_back("state");
        } else {
            vector<TData> dlist;
            ret = Api->Dlist(dlist);
            if (ret) {
                PrintError("Can't list data");
                return EXIT_FAILURE;
            }

            vector<TProperty> plist;
            ret = Api->Plist(plist);
            if (ret) {
                PrintError("Can't list properties");
                return EXIT_FAILURE;
            }

            for (int i = 0; i < argc; i++) {
                string arg = argv[i];

                if (!ValidData(dlist, arg) && !ValidProperty(plist, arg)) {
                    TError error(EError::InvalidValue, "Invalid value");
                    PrintError(error, "Can't parse argument");
                    return EXIT_FAILURE;
                }

                showData.push_back(arg);
            }
        }

        string sortBy = showData[0];
        size_t nameLen = CalculateFieldLength(clist, strlen("container"));

        for (auto container : clist) {
            string state;
            ret = Api->GetData(container, "state", state);
            if (ret) {
                PrintError("Can't get container state");
                return EXIT_FAILURE;
            }

            if (state != "running" && state != "dead")
                continue;

            map<string, string> dataVal;
            for (auto data : showData) {
                string val;
                if (Api->GetData(container, data, val))
                    (void)Api->GetProperty(container, data, val);
                dataVal[data] = val;
            }

            containerData.push_back(make_pair(container, dataVal));
        }

        std::sort(containerData.begin(), containerData.end(),
                  [&](pair<string, map<string, string>> a,
                     pair<string, map<string, string>> b) {
                  string as, bs;
                  int64_t an, bn;

                  if (a.second.find(sortBy) != a.second.end())
                      as = a.second[sortBy];

                  if (b.second.find(sortBy) != b.second.end())
                      bs = b.second[sortBy];

                  TError aError = StringToInt64(as, an);
                  TError bError = StringToInt64(bs, bn);
                  if (aError || bError)
                      return as > bs;

                  return an > bn;
                  });

        vector<size_t> fieldLen;
        for (auto &data : showData) {
            vector<string> tmp;
            tmp.push_back(data);

            for (auto &pair : containerData)
                tmp.push_back(HumanValue(data, pair.second[data]));

            fieldLen.push_back(CalculateFieldLength(tmp));
        }

        std::cout << std::left << std::setw(nameLen) << "container";
        for (size_t i = 0; i < showData.size(); i++)
            std::cout << std::right << std::setw(fieldLen[i]) << showData[i];
        std::cout << std::endl;

        for (auto &pair : containerData) {
            std::cout << std::left << std::setw(nameLen) << pair.first;

            for (size_t i = 0; i < showData.size(); i++) {
                std::cout << std::right << std::setw(fieldLen[i]);
                std::cout << HumanValue(showData[i], pair.second[showData[i]]);
            }
            std::cout << std::endl;
        }

        return ret;
    }
};

class TCreateVolumeCmd : public ICmd {
public:
    TCreateVolumeCmd(TPortoAPI *api) : ICmd(api, "vcreate", 2,
                                            "<path> <source> [quota] [flags...]", "create volume") {}

    int Execute(int argc, char *argv[]) {
        std::string flags = (argc == 4 ? argv[3] : "");
        std::string quota = (argc >= 3 ? argv[2] : "0");
        int ret = Api->CreateVolume(argv[0], argv[1], quota, flags);
        if (ret) {
            PrintError("Can't create volume");
            return ret;
        }

        return 0;
    }
};

class TDestroyVolumeCmd : public ICmd {
public:
    TDestroyVolumeCmd(TPortoAPI *api) : ICmd(api, "vdestroy", 1, "<path> [path...]", "destroy volume") {}

    int Execute(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
            int ret = Api->DestroyVolume(argv[i]);
            if (ret) {
                PrintError("Can't destroy volume");
                return ret;
            }
        }

        return 0;
    }
};

class TListVolumesCmd : public ICmd {
public:
    TListVolumesCmd(TPortoAPI *api) : ICmd(api, "vlist", 0, "", "list created volumes") {}

    int Execute(int argc, char *argv[]) {
        vector<TVolumeDescription> vlist;
        int ret = Api->ListVolumes(vlist);
        if (ret) {
            PrintError("Can't list volumes");
            return ret;
        }

        for (auto v : vlist) {
            std::cout << v.Path << " "
                      << v.Source << " "
                      << v.Quota << " "
                      << v.Flags << " "
                      << "usage: " << v.Used << "/" << v.Avail << " (" << (v.Used * 100 / v.Avail) << "%) "
                      << std::endl;
        }

        return EXIT_SUCCESS;
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
    RegisterCommand(new TRestartCmd(&api));
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
    RegisterCommand(new TExecCmd(&api));

    RegisterCommand(new TCreateVolumeCmd(&api));
    RegisterCommand(new TDestroyVolumeCmd(&api));
    RegisterCommand(new TListVolumesCmd(&api));

    TLogger::DisableLog();

    return HandleCommand(&api, argc, argv);
};
