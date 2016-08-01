#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <csignal>
#include <cmath>

#include "libporto.hpp"
#include "cli.hpp"
#include "volume.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/namespace.hpp"
#include "util/log.hpp"
#include "util/cred.hpp"

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

static int ForwardPtyMaster;

static void ForwardWinch(int sig) {
    struct winsize winsize;

    /* Copy window size into master terminal */
    if (!ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize))
        (void)ioctl(ForwardPtyMaster, TIOCSWINSZ, &winsize);
}

volatile int ChildDead;

static void CatchChild(int sig) {
    ChildDead = 1;
}

class TLauncher {
public:
    Porto::Connection *Api;
    TLauncher(Porto::Connection *api) : Api(api) {}

    bool WeakContainer = false;
    bool StartContainer = true;
    bool NeedVolume = false;
    bool ChrootVolume = true;
    bool MergeLayers = false;
    bool StartOS = false;
    bool ForwardTerminal = false;
    bool ForwardStreams = false;
    bool WaitExit = false;

    std::string Container;
    std::vector<std::pair<std::string, std::string>> Properties;
    std::vector<std::string> Environment;

    Porto::Volume Volume;
    std::string SpaceLimit;
    std::string VolumeBackend;
    std::string VolumeStorage;
    std::vector<std::string> Layers;
    std::vector<std::string> VolumeLayers;
    std::vector<std::string> ImportedLayers;
    bool ContainerCreated = false;
    bool VolumeLinked = false;
    int LayerIndex = 0;

    int MasterPty = -1;
    int SlavePty = -1;

    int WaitTimeout = -1;

    int ExitCode = -1;
    int ExitSignal = -1;
    std::string ExitMessage = "";

    ~TLauncher() {
        CloseSlavePty();
        CloseMasterPty();
    }

    TError GetLastError() {
        int error;
        std::string msg;
        Api->GetLastError(error, msg);
        return TError((EError)error, msg);
    }

    TError SetProperty(const std::string &key, const std::string &val) {

        if (key == "virt_mode")
            StartOS = val == "os";

        if (key == "env") {
            Environment.push_back(val);
        } else if (key == "space_limit") {
            SpaceLimit = val;
            NeedVolume = true;
        } else if (key == "backend") {
            VolumeBackend = val;
        } else if (key == "storage") {
            VolumeStorage = val;
        } else if (key == "layers") {
            NeedVolume = true;
            return SplitEscapedString(val, ';', Layers);
        } else
            Properties.emplace_back(key, val);

        return TError::Success();
    }

    TError SetProperty(const std::string &prop) {
        std::string::size_type n = prop.find('=');
        if (n == std::string::npos)
            return TError(EError::InvalidValue, "Invalid value: " + prop);
        std::string key = prop.substr(0, n);
        std::string val = prop.substr(n + 1);
        return SetProperty(key, val);
    }

    TError ImportLayer(const TPath &path, std::string &id) {
        id = "_weak_portoctl-" + std::to_string(GetPid()) + "-" +
             std::to_string(LayerIndex++) + "-" + path.BaseName();
        std::cerr << "Importing layer " << path << " as " << id << std::endl;
        if (Api->ImportLayer(id, path.ToString()))
            return GetLastError();
        ImportedLayers.push_back(id);
        return TError::Success();
    }

    TError ImportLayers() {
        std::vector<std::string> known;
        TError error;

        if (Api->ListLayers(known))
            return GetLastError();

        for (auto &layer : Layers) {
            if (std::find(known.begin(), known.end(), layer) != known.end()) {
                VolumeLayers.push_back(layer);
                continue;
            }

            auto path = TPath(layer).RealPath();

            if (path.IsDirectoryFollow()) {
                VolumeLayers.push_back(path.ToString());
            } else if (path.IsRegularFollow()) {
                std::string id;
                error = ImportLayer(path, id);
                if (error)
                    break;
                VolumeLayers.push_back(id);
            } else
                return TError(EError::LayerNotFound, layer);
        }

        return error;
    }

    TError CreateVolume() {
        std::map<std::string, std::string> config;
        TError error;

        if (SpaceLimit != "")
            config["space_limit"] = SpaceLimit;

        if (!Layers.empty()) {
            error = ImportLayers();
            if (error)
                return error;
            config["layers"] = CommaSeparatedList(VolumeLayers, ";");
        }

        if (VolumeBackend != "")
            config["backend"] = VolumeBackend;
        else if (MergeLayers || Layers.empty())
            config["backend"] = "native";
        else
            config["backend"] = "overlay";

        if (VolumeStorage != "")
            config["storage"] = VolumeStorage;

        if (Api->CreateVolume("", config, Volume))
            return GetLastError();
        VolumeLinked = true;

        if (Container != "") {
            if (Api->LinkVolume(Volume.Path, Container))
                return GetLastError();
            if (Api->UnlinkVolume(Volume.Path, ""))
                return GetLastError();
            VolumeLinked = false;
        }

        return TError::Success();
    }

    TError WaitContainer(int timeout) {
        std::vector<std::string> containers = { Container };
        std::string result;
        TError error;
        int status;

        if (Api->WaitContainers(containers, result, timeout))
            return GetLastError();

        if (result == "")
            return TError(EError::Busy, "Wait timeout");

        if (Api->GetData(Container, "exit_status", result))
            return GetLastError();

        error = StringToInt(result, status);
        if (!error) {
            if (WIFSIGNALED(status)) {
                ExitSignal = WTERMSIG(status);
                ExitMessage = StringFormat("Container killed by signal: %d (%s)",
                                            ExitSignal, strsignal(ExitSignal));
            } else if (WIFEXITED(status)) {
                ExitCode = WEXITSTATUS(status);
                ExitMessage = StringFormat("Container exit code: %d", ExitCode);
            }
        }
        return error;
    }

    TError OpenPty() {
        MasterPty = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (MasterPty < 0)
            return TError(EError::Unknown, errno, "Cannot open master terminal");

        char slave[128];
        if (ptsname_r(MasterPty, slave, sizeof(slave)))
            return TError(EError::Unknown, errno, "Cannot get terminal name");

        if (unlockpt(MasterPty))
            return TError(EError::Unknown, errno, "Cannot unlock terminal");

        SlavePty = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (SlavePty < 0)
            return TError(EError::Unknown, errno, "Cannot open slave terminal");

        return TError::Success();
    }

    void CloseSlavePty() {
        if (SlavePty >= 0)
            close(SlavePty);
        SlavePty = -1;
    }

    void CloseMasterPty() {
        if (MasterPty >= 0)
            close(MasterPty);
        MasterPty = -1;
    }

    TError ForwardPty() {
        struct termios termios;
        char buf[4096];
        ssize_t nread;

        CloseSlavePty();

        ForwardPtyMaster = MasterPty;
        Signal(SIGWINCH, ForwardWinch);
        ForwardWinch(SIGWINCH);

        /* switch outer terminal into raw mode, disable echo, etc */
        if (!tcgetattr(STDIN_FILENO, &termios)) {
            struct termios raw = termios;

            cfmakeraw(&raw);
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }

        ChildDead = 0;
        Signal(SIGCHLD, CatchChild);

        pid_t pid = fork();

        if (pid < 0)
            return TError(EError::Unknown, errno, "cannot fork");

        if (!pid) {
            while (1) {
                if ((nread = read(MasterPty, &buf, sizeof buf)) <= 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    break;
                }
                if (write(STDOUT_FILENO, &buf, nread) < 0)
                    break;
            }
            exit(0);
        } else {
            int escape = 0;

            while (!ChildDead) {
                if ((nread = read(STDIN_FILENO, &buf, sizeof buf)) <= 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    break;
                }

                for (int i = 0; i < nread; i++) {
                    if (buf[i] == '\3')
                        escape++;
                    else
                        escape = 0;
                }

                if (escape >= 7) {
                    if (Api->Kill(Container, 9))
                        std::cerr << "Cannot kill container : " << GetLastError() << std::endl;
                    break;
                }

                if (write(MasterPty, &buf, nread) < 0)
                    break;
            }

            kill(pid, SIGKILL);
        }

        /* restore state of outer terminal */
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &termios);

        Signal(SIGWINCH, SIG_DFL);
        Signal(SIGCHLD, SIG_DFL);

        CloseMasterPty();

        return TError::Success();
    }

    TError ApplyConfig() {

        if (StartOS && Api->SetProperty(Container, "virt_mode", "os"))
            goto err;

        if (ForwardTerminal) {
            std::string tty = "/dev/fd/" + std::to_string(SlavePty);
            if (Api->SetProperty(Container, "stdin_path", tty) ||
                    Api->SetProperty(Container, "stdout_path", tty) ||
                    Api->SetProperty(Container, "stderr_path", tty))
                goto err;
        } else if (ForwardStreams) {
            if (Api->SetProperty(Container, "stdin_path", "/dev/fd/0") ||
                    Api->SetProperty(Container, "stdout_path", "/dev/fd/1") ||
                    Api->SetProperty(Container, "stderr_path", "/dev/fd/2"))
                goto err;
        }

        if (NeedVolume) {
            if (Api->SetProperty(Container, "root", ChrootVolume ? Volume.Path : "/"))
                goto err;
            if (Api->SetProperty(Container, "cwd", ChrootVolume ? "/" : Volume.Path))
                goto err;
        }

        for (auto &prop : Properties) {
            if (Api->SetProperty(Container, prop.first, prop.second))
                goto err;
        }

        if (Api->SetProperty(Container, "env", CommaSeparatedList(Environment, ";")))
            goto err;

        return TError::Success();
err:
        return GetLastError();
    }

    TError Launch() {
        TError error;

        if (WeakContainer) {
            if (Api->CreateWeakContainer(Container))
                return GetLastError();
        } else {
            if (Api->Create(Container))
                return GetLastError();
        }
        ContainerCreated = true;

        if (NeedVolume) {
            error = CreateVolume();
            if (error)
                goto err;
        }

        /* forward terminal only if all streams are tty */
        if (ForwardTerminal)
             ForwardTerminal = isatty(STDIN_FILENO) &&
                 isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);

        if (ForwardTerminal) {
            error = OpenPty();
            if (error)
                goto err;
            std::string term = getenv("TERM") ?: "xterm";
            Environment.push_back("TERM=" + term);
        }

        error = ApplyConfig();
        if (error)
            goto err;

        if (StartContainer) {
            if (Api->Start(Container)) {
                error = GetLastError();
                goto err;
            }
        }

        if (ForwardTerminal) {
            error = ForwardPty();
            if (error)
                goto err;
        }

        if (WaitExit) {
            error = WaitContainer(WaitTimeout);
            if (error)
                goto err;
        }

        return TError::Success();

err:
        Cleanup();
        return error;
    }

    TError StopContainer() {
        if (Api->Stop(Container)) {
            std::cerr << "Cannot stop container " << Container << " : " << GetLastError() << std::endl;
            return GetLastError();
        }
        return TError::Success();
    }

    void Cleanup() {
        if (ContainerCreated) {
            if (Api->Destroy(Container))
                std::cerr << "Cannot destroy container " << Container << " : " << GetLastError() << std::endl;
            ContainerCreated = false;
        }

        if (VolumeLinked) {
            if (Api->UnlinkVolume(Volume.Path, ""))
                std::cerr << "Cannot unlink volume " << Volume.Path << " : " << GetLastError() << std::endl;
            VolumeLinked = false;
        }

        for (auto &layer : ImportedLayers) {
            if (Api->RemoveLayer(layer) && GetLastError().GetError() != EError::LayerNotFound)
                std::cerr << "Cannot remove layer " << layer << " : " << GetLastError() << std::endl;
        }
        ImportedLayers.clear();
        VolumeLayers.clear();
        CloseSlavePty();
        CloseMasterPty();
    }
};

static string HumanNsec(const string &val) {
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

static string HumanSec(const string &val) {
    uint64_t n = stoull(val);
    uint64_t h = 0, m = 0, s = n;

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

static string HumanSize(const string &val) {
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

static const std::string StripIdx(const std::string &name) {
    if (name.find('[') != std::string::npos)
        return std::string(name.c_str(), name.find('['));
    else
        return name;
}

static bool ValidData(const vector<Porto::Property> &dlist, const string &name) {
    return find_if(dlist.begin(), dlist.end(),
                   [&](const Porto::Property &i)->bool { return i.Name == StripIdx(name); })
        != dlist.end();
}

static bool ValidProperty(const vector<Porto::Property> &plist, const string &name) {
    return find_if(plist.begin(), plist.end(),
                   [&](const Porto::Property &i)->bool { return i.Name == StripIdx(name); })
        != plist.end();
}

static string HumanValue(const string &name, const string &val) {
    if (val == "")
        return val;

    if (name == "memory_guarantee" || name == "memory_limit" ||
            name == "anon_usage" || name == "anon_limit") {
        return HumanSize(val);
    } else if (name == "exit_status") {
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
    } else if (name == "memory_usage" || name == "max_rss") {
        return HumanSize(val);
    } else if (name == "cpu_usage" || name == "cpu_usage_system") {
        return HumanNsec(val);
    } else if (name == "time") {
        return HumanSec(val);
    }

    return val;
}

class TRawCmd final : public ICmd {
public:
    TRawCmd(Porto::Connection *api) : ICmd(api, "raw", 1, "<message>", "send raw protobuf message") {}

    int Execute(TCommandEnviroment *env) final override {
        stringstream msg;

        const auto &args = env->GetArgs();
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

        string resp;
        if (!Api->Raw(msg.str(), resp))
            std::cout << resp << std::endl;

        return 0;
    }
};

class TCreateCmd final : public ICmd {
public:
    TCreateCmd(Porto::Connection *api) : ICmd(api, "create", 1, "<container1> [container2...]", "create container") {}

    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Create(arg);
            if (ret) {
                PrintError("Can't create container");
                return ret;
            }
        }

        return 0;
    }
};

class TGetPropertyCmd final : public ICmd {
public:
    TGetPropertyCmd(Porto::Connection *api) : ICmd(api, "pget", 2, "[-k] <container> <property> [property...]", "get raw container property") {}

    int Execute(TCommandEnviroment *env) final override {
        bool printKey = false;
        const auto &args = env->GetOpts({
            { 'k', false, [&](const char *arg) { printKey = true; } },
        });

        for (size_t i = 1; i < args.size(); ++i) {
            string value;
            int ret = Api->GetProperty(args[0], args[i], value);
            if (ret) {
                PrintError("Can't get property");
                return ret;
            }
            if (printKey)
                PrintPair(args[i], value);
            else
                Print(value);
        }

        return 0;
    }
};

class TSetPropertyCmd final : public ICmd {
public:
    TSetPropertyCmd(Porto::Connection *api) : ICmd(api, "set", 3, "<container> <property>", "set container property") {}

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        PORTO_ASSERT(args.size() >= static_cast<size_t>(NeedArgs));
        string val = args[2];
        for (size_t i = 3; i < args.size(); ++i) {
            val += " ";
            val += args[i];
        }

        int ret = Api->SetProperty(args[0], args[1], val);
        if (ret)
            PrintError("Can't set property");

        return ret;
    }
};

class TGetDataCmd final : public ICmd {
public:
    TGetDataCmd(Porto::Connection *api) : ICmd(api, "dget", 2, "[-k] <container> <data> [data...]", "get raw container data") {}

    int Execute(TCommandEnviroment *env) final override {
        bool printKey = false;
        const auto &args = env->GetOpts({
            { 'k', false, [&](const char *arg) { printKey = true; } },
        });

        for (size_t i = 1; i < args.size(); ++i) {
            string value;
            int ret = Api->GetData(args[0], args[i], value);
            if (ret) {
                PrintError("Can't get data");
                return ret;
            }
            if (printKey)
                PrintPair(args[i], value);
            else
                Print(value);
        }

        return 0;
    }
};

class TStartCmd final : public ICmd {
public:
    TStartCmd(Porto::Connection *api) : ICmd(api, "start", 1, "<container1> [container2...]", "start container") {}

    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Start(arg);
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

class TKillCmd final : public ICmd {
public:
    TKillCmd(Porto::Connection *api) : ICmd(api, "kill", 1, "<container> [signal]", "send signal to container") {}

    int Execute(TCommandEnviroment *env) final override {
        int sig = SIGTERM;
        const auto &args = env->GetArgs();
        PORTO_ASSERT(!args.empty());
        if (args.size() >= 2) {
            const string &sigName = args[1];

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

        int ret = Api->Kill(args[0], sig);
        if (ret)
            PrintError("Can't send signal to container");

        return ret;
    }
};

class TStopCmd final : public ICmd {
public:
    TStopCmd(Porto::Connection *api) : ICmd(api, "stop", 1, "[-T <seconds>] <container1> [container2...]", "stop container",
             "    -T <seconds> per-container stop timeout\n") {}

    int Execute(TCommandEnviroment *env) final override {
        int timeout = -1;

        const auto &containers = env->GetOpts({
            { 'T', true, [&](const char *arg) { timeout = std::stoi(arg); } },
        });

        for (const auto &arg : containers) {
            int ret = Api->Stop(arg, timeout);
            if (ret) {
                PrintError("Can't stop container");
                return ret;
            }
        }

        return 0;
    }
};

class TRestartCmd final : public ICmd {
public:
    TRestartCmd(Porto::Connection *api) : ICmd(api, "restart", 1, "<container1> [container2...]", "restart container") {}

    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Stop(arg);
            if (ret) {
                PrintError("Can't stop container");
                return ret;
            }

            ret = Api->Start(arg);
            if (ret) {
                PrintError("Can't start container");
                return ret;
            }
        }

        return 0;
    }
};

class TPauseCmd final : public ICmd {
public:
    TPauseCmd(Porto::Connection *api) : ICmd(api, "pause", 1, "<container> [name...]", "pause container") {}

    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Pause(arg);
            if (ret) {
                PrintError("Can't pause container");
                return ret;
            }
        }

        return 0;
    }
};

class TResumeCmd final : public ICmd {
public:
    TResumeCmd(Porto::Connection *api) : ICmd(api, "resume", 1, "<container1> [container2...]", "resume container") {}

    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Resume(arg);
            if (ret) {
                PrintError("Can't resume container");
                return ret;
            }
        }

        return 0;
    }
};

class TGetCmd final : public ICmd {
public:
    TGetCmd(Porto::Connection *api) : ICmd(api, "get", 1, "<container> <variable> [variable...]", "get container property or data") {}

    int Execute(TCommandEnviroment *env) final override {
        string value;
        int ret;
        bool printKey = true;
        bool printErrors = true;

        vector<Porto::Property> plist;
        ret = Api->Plist(plist);
        if (ret) {
            PrintError("Can't list properties");
            return EXIT_FAILURE;
        }

        vector<Porto::Property> dlist;
        ret = Api->Dlist(dlist);
        if (ret) {
            PrintError("Can't list data");
            return EXIT_FAILURE;
        }

        const auto &args = env->GetArgs();
        std::string container = args[0];
        std::vector<std::string> clist = { container };
        std::vector<std::string> vars;

        if (args.size() > 1) {
            vars.insert(vars.end(), args.begin() + 1, args.end());
            // we want to preserve old behavior:
            // - get without arguments prints all properties/data prefixed with name
            // - get with arguments prints specified properties/data without prefix
            printKey = false;
        } else {
            vars.reserve(plist.size() + dlist.size());
            for (auto p : plist)
                vars.push_back(p.Name);
            for (auto d : dlist)
                vars.push_back(d.Name);
            std::sort(vars.begin(), vars.end());
            // don't print any error when user lists all properties/data
            // (container may be / which doesn't support properties or
            // some property/data may be invalid in given state)
            printErrors = false;
        }

        std::map<std::string, std::map<std::string, Porto::GetResponse>> result;
        ret = Api->Get(clist, vars, result);
        if (ret) {
            PrintError("Can't get containers' data");
            return ret;
        }

        auto &data = result[container];
        ret = EXIT_SUCCESS;

        for (const auto &key : vars) {
            if (data[key].Error) {
                if (printErrors || key == "state") {
                    TError error((rpc::EError)data[key].Error, data[key].ErrorMsg);
                    PrintError(error, "Can't get " + key);
                    ret = EXIT_FAILURE;
                }
                continue;
            }

            auto val = HumanValue(key, data[key].Value);
            if (printKey)
                PrintPair(key, val);
            else
                Print(val);
        }

        return ret;
    }
};

class TEnterCmd final : public ICmd {
public:
    TEnterCmd(Porto::Connection *api) : ICmd(api, "enter", 1,
            "[-C] <container> [command]",
            "execute command in container namespace",
            "    -C          do not enter cgroups\n"
            "                default command is /bin/bash\n"
            ) {}

    void PrintErrno(const string &str) {
        std::cerr << str << ": " << strerror(errno) << std::endl;
    }

    TError GetCgMount(const string &subsys, TPath &root) {
        vector<string> subsystems;
        TError error = SplitString(subsys, ',', subsystems);
        if (error)
            return error;

        std::list<TMount> mounts;
        error = TPath::ListAllMounts(mounts);
        if (error)
            return error;

        for (auto &mnt : mounts) {
            if (mnt.Type != "cgroup")
                continue;

            bool found = true;
            for (auto &ss : subsystems) {
                if (!mnt.HasOption(ss)) {
                    found = false;
                    break;
                }
            }

            if (found) {
                root = mnt.Target;
                return TError::Success();
            }
        }

        return TError(EError::Unknown, "Can't find root for " + subsys);
    }

    int Execute(TCommandEnviroment *env) final override {
        bool enterCgroups = true;
        const auto &args = env->GetOpts({
            { 'C', false, [&](const char *arg) { enterCgroups = false; } },
        });

        string cmd;
        for (size_t i = 1; i < args.size(); ++i) {
            cmd += args[i];
            cmd += " ";
        }

        if (!cmd.length())
            cmd = "/bin/bash";

        string pidStr;
        int ret = Api->GetData(args[0], "root_pid", pidStr);
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

        if (!pid) {
            std::cerr << "Task pid in this namespace is unknown." << std::endl;
            std::cerr << "Try enter parent container or enter from host." << std::endl;
            return EXIT_FAILURE;
        }

        TNamespaceSnapshot ns;
        error = ns.Open(pid);
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
                TPath root;
                TError error = GetCgMount(cg.first, root);
                if (error) {
                    PrintError(error, "Cannot find cgroup mounts, try option \"-C\"");
                    return EXIT_FAILURE;
                }

                error = TPath(root / cg.second / "cgroup.procs").WriteAll(std::to_string(GetPid()));
                if (error) {
                    PrintError(error, "Cannot enter cgroups, try option \"-C\"");
                    return EXIT_FAILURE;
                }
            }
        }

        error = ns.Enter();
        if (error) {
            PrintError(error, "Cannot enter namespaces");
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

class TRunCmd final : public ICmd {
public:
    TRunCmd(Porto::Connection *api) : ICmd(api, "run", 2,
            "[-L layer]... <container> [properties]",
            "create and start container with given properties",
            "    -L layer|dir|tarball        add lower layer (-L top ... -L bottom)\n")
    {}

    int Execute(TCommandEnviroment *env) final override {
        TLauncher launcher(Api);
        TError error;

        const auto &args = env->GetOpts({
            { 'L', true, [&](const char *arg) {
                    launcher.Layers.push_back(arg);
                    launcher.NeedVolume = true;
                }
            },
        });

        launcher.Container = args[0];
        for (size_t i = 1; i < args.size(); ++i) {
            error = launcher.SetProperty(args[i]);
            if (error) {
                std::cerr << "Cannot set property: " << error << std::endl;
                return EXIT_FAILURE;
            }
        }

        error = launcher.Launch();
        if (error) {
            std::cerr << "Cannot start container: " << error << std::endl;
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }
};

class TExecCmd final : public ICmd {
public:
    TExecCmd(Porto::Connection *api) : ICmd(api, "exec", 2,
        "[-C] [-T] [-L layer]... <container> command=<command> [properties]",
        "Execute command in container, forward terminal, destroy container at the end",
        "    -L layer|dir|tarball        add lower layer (-L top ... -L bottom)\n"
        ) { }

    int Execute(TCommandEnviroment *environment) final override {
        TLauncher launcher(Api);
        TError error;

        launcher.WeakContainer = true;
        launcher.ForwardTerminal = true;
        launcher.ForwardStreams = true;
        launcher.WaitExit = true;

        const auto &args = environment->GetOpts({
            { 'C', false, [&](const char *arg) { launcher.WeakContainer = false; } },
            { 'T', false, [&](const char *arg) { launcher.ForwardTerminal = false; } },
            { 'L', true, [&](const char *arg) { launcher.Layers.push_back(arg); launcher.NeedVolume = true; } },
        });

        launcher.Container = args[0];
        for (size_t i = 1; i < args.size(); ++i) {
            error = launcher.SetProperty(args[i]);
            if (error) {
                std::cerr << "Cannot set property: " << error << std::endl;
                return EXIT_FAILURE;
            }
        }

        error = launcher.Launch();
        if (error) {
            std::cerr << "Cannot start container: " << error << std::endl;
            return EXIT_FAILURE;
        }

        if (launcher.WeakContainer)
            launcher.Cleanup();

        if (launcher.ExitSignal > 0) {
            std::cerr << launcher.ExitMessage << std::endl;
            return 128 + launcher.ExitSignal;
        }

        if (launcher.ExitCode > 0) {
            std::cerr << launcher.ExitMessage << std::endl;
            return launcher.ExitCode;
        }

        return EXIT_SUCCESS;
    }
};

class TShellCmd final : public ICmd {
public:
    TShellCmd(Porto::Connection *api) : ICmd(api, "shell", 1,
            "[-u <user>] [-g <group>] <container> [command] [argument]...",
            "start shell (default /bin/bash) in container") { }

    int Execute(TCommandEnviroment *environment) final override {
        std::string current_user = getenv("SUDO_USER") ?: getenv("USER") ?: "unknown";
        std::string command = "/bin/bash";
        std::string user, group;

        const auto &args = environment->GetOpts({
            { 'u', true, [&](const char *arg) { user = arg; } },
            { 'g', true, [&](const char *arg) { group = arg; } },
        });

        TLauncher launcher(Api);
        TError error;

        if (args.size() < 1) {
            PrintUsage();
            return EXIT_FAILURE;
        }

        if (args.size() > 1) {
            command = args[1];
            for (size_t i = 2; i < args.size(); ++i)
                command += " " + args[i];
        }

        launcher.WeakContainer = true;
        launcher.ForwardTerminal = true;
        launcher.ForwardStreams = true;
        launcher.WaitExit = true;

        launcher.Container = args[0] + "/shell-" + current_user + "-" + std::to_string(GetPid());
        launcher.SetProperty("command", command);
        launcher.SetProperty("isolate", "false");
        launcher.Environment.push_back("debian_chroot=" + args[0]);

        if (user != "") {
            if (user == "root") {
                launcher.SetProperty("virt_mode", "os");
                launcher.SetProperty("net", "inherited");
            } else
                launcher.SetProperty("user", user);

            if (group == "") {
                TCred cred;
                if (!cred.Load(user))
                    group = cred.Group();
            }
        }

        if (group != "" && (user != "root" || group != "root"))
            launcher.SetProperty("group", group);

        error = launcher.Launch();
        if (error) {
            std::cerr << "Cannot start container: " << error << std::endl;
            return EXIT_FAILURE;
        }

        launcher.Cleanup();

        if (launcher.ExitSignal > 0) {
            std::cerr << launcher.ExitMessage << std::endl;
            return 128 + launcher.ExitSignal;
        }

        if (launcher.ExitCode > 0) {
            std::cerr << launcher.ExitMessage << std::endl;
            return launcher.ExitCode;
        }

        return EXIT_SUCCESS;
    }
};

class TGcCmd final : public ICmd {
public:
    TGcCmd(Porto::Connection *api) : ICmd(api, "gc", 0, "", "remove all dead containers") {}

    int Execute(TCommandEnviroment *env) final override {
        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        for (const auto &c : clist) {
            if (c == "/")
                continue;

            string state;
            ret = Api->GetData(c, "state", state);
            if (ret) {
                PrintError("Can't get container state");
                continue;
            }

            if (state != "dead")
                continue;

            int ret = Api->Destroy(c);
            if (ret) {
                PrintError("Can't destroy container");
                return ret;
            }
        }

        return EXIT_SUCCESS;
    }
};

class TFindCmd final : public ICmd {
public:
    TFindCmd(Porto::Connection *api) : ICmd(api, "find", 1, "", "find container for given process id") {}

    int Execute(TCommandEnviroment *env) final override {
        int pid;
        const auto &args = env->GetArgs();
        TError error = StringToInt(args[0], pid);
        if (error) {
            std::cerr << "Can't parse pid " << args[0] << std::endl;
            return EXIT_FAILURE;
        }

        std::map<std::string, std::string> cgmap;
        error = GetTaskCgroups(pid, cgmap);
        if (error) {
            std::cerr << "Can't read /proc/" << pid << "/cgroup, is process alive?" << std::endl;
            return EXIT_FAILURE;
        }

        if (cgmap.find("freezer") == cgmap.end()) {
            std::cerr << "Process " << pid << " is not part of freezer cgroup" << std::endl;
            return EXIT_FAILURE;
        }

        auto freezer = cgmap["freezer"];
        auto prefix = std::string(PORTO_ROOT_CGROUP) + "/";
        if (freezer.length() < prefix.length() || freezer.substr(0, prefix.length()) != prefix) {
            std::cerr << "Process " << pid << " is not managed by porto" << std::endl;
            return EXIT_FAILURE;
        }

        Print(freezer.replace(0, prefix.length(), ""));

        return EXIT_SUCCESS;
    }
};

class TDestroyCmd final : public ICmd {
public:
    TDestroyCmd(Porto::Connection *api) : ICmd(api, "destroy", 1, "<container1> [container2...]", "destroy container") {}

    int Execute(TCommandEnviroment *env) final override {
        int exitStatus = EXIT_SUCCESS;
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Destroy(arg);
            if (ret) {
                PrintError("Can't destroy container");
                exitStatus = ret;
            }
        }

        return exitStatus;
    }
};

class TWaitCmd final : public ICmd {
public:
    TWaitCmd(Porto::Connection *api) : ICmd(api, "wait", 0,
             "[-T <seconds>] <container|wildcard> ...",
             "Wait for any listed container change state to dead or meta without running children",
             "    -T <seconds>  timeout\n"
             ) {}

    int Execute(TCommandEnviroment *env) final override {
        int timeout = -1;
        const auto &containers = env->GetOpts({
            { 't', true, [&](const char *arg) { timeout = (std::stoi(arg) + 999) / 1000; } },
            { 'T', true, [&](const char *arg) { timeout = std::stoi(arg); } },
        });

        if (containers.empty()) {
            PrintUsage();
            return EXIT_FAILURE;
        }

        std::string name;
        int ret = Api->WaitContainers(containers, name, timeout);
        if (ret) {
            PrintError("Can't wait for containers");
            return ret;
        }

        if (name.empty())
            std::cerr << "timeout" << std::endl;
        else
            std::cout << name << std::endl;

        return 0;
    }
};

class TListCmd final : public ICmd {
public:
    TListCmd(Porto::Connection *api) : ICmd(api, "list", 0, "[-1] [-f] [-t]", "list created containers") {}

    size_t CountChar(const std::string &s, const char ch) {
        size_t count = 0;
        for (size_t i = 0; i < s.length(); i++)
            if (s[i] == ch)
                count++;
        return count;
    }

    std::string GetParent(const std::string &child) {
        auto lastSlash = child.rfind("/");
        if (lastSlash == std::string::npos)
            return "/";
        else
            return child.substr(0, lastSlash);
    }

    int Execute(TCommandEnviroment *env) final override {
        bool details = true;
        bool forest = false;
        bool toplevel = false;
        (void)env->GetOpts({
            { '1', false, [&](const char *arg) { details = false; } },
            { 'f', false, [&](const char *arg) { forest = true; } },
            { 't', false, [&](const char *arg) { toplevel = true; } },
        });

        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        vector<string> displayName;
        std::copy(clist.begin(), clist.end(), std::back_inserter(displayName));

        if (forest)
            for (size_t i = 0; i < clist.size(); i++) {
                auto c = clist[i];

                string parent = GetParent(c);
                if (parent != "/") {
                    string prefix = " ";
                    for (size_t j = 1; j < CountChar(displayName[i], '/'); j++)
                            prefix = prefix + "   ";

                    displayName[i] = prefix + "\\_ " + displayName[i].substr(parent.length() + 1);
                }
            }

        const std::vector<std::string> vars = { "state", "time" };
        std::map<std::string, std::map<std::string, Porto::GetResponse>> result;
        ret = Api->Get(clist, vars, result);
        if (ret) {
            PrintError("Can't get containers' data");
            return ret;
        }

        vector<string> states = { "running", "dead", "meta", "stopped", "paused" };
        size_t stateLen = MaxFieldLength(states);
        size_t nameLen = MaxFieldLength(displayName);
        size_t timeLen = 12;
        for (size_t i = 0; i < clist.size(); i++) {
            auto c = clist[i];
            if (c == "/")
                continue;

            auto state = result[c]["state"];
            if (state.Error == EError::ContainerDoesNotExist)
                continue;

            if (toplevel && CountChar(c, '/'))
                continue;

            if (details)
                std::cout << std::left << std::setw(nameLen);

            std::cout << displayName[i];

            if (details) {
                if (state.Error == EError::Busy)
                    std::cout << std::right << std::setw(stateLen) << "busy";
                else if (state.Error)
                    std::cout << std::right << std::setw(stateLen) << state.ErrorMsg;
                else
                    std::cout << std::right << std::setw(stateLen) << state.Value;

                auto time = result[c]["time"];
                bool showTime = state.Value == "running" ||
                                state.Value == "meta" ||
                                state.Value == "dead";
                if (showTime && !time.Error)
                    std::cout << std::right << std::setw(timeLen)
                              << HumanValue("time", time.Value);
            }

            std::cout << std::endl;
        }

        return EXIT_SUCCESS;
    }
};

extern int portotop(Porto::Connection *api, std::string config);
class TTopCmd final : public ICmd {
public:
    TTopCmd(Porto::Connection *api) : ICmd(api, "top", 0, "[config]", "top-like tool for container monitoring and control") {}

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        if (args.empty())
            return portotop(Api, "");
        else
            return portotop(Api, args[0]);
    }
};

class TSortCmd final : public ICmd {
public:
    TSortCmd(Porto::Connection *api) : ICmd(api, "sort", 0, "[sort-by]", "print containers sorted by resource usage") {}

    int Execute(TCommandEnviroment *env) final override {
        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return EXIT_FAILURE;
        }

        vector<pair<string, map<string, string>>> containerData;
        vector<string> showData = env->GetArgs();

        if (showData.empty()) {
            showData.push_back("cpu_usage");
            showData.push_back("memory_usage");
            showData.push_back("major_faults");
            showData.push_back("minor_faults");
            showData.push_back("net_packets");
            showData.push_back("state");
        } else {
            vector<Porto::Property> dlist;
            ret = Api->Dlist(dlist);
            if (ret) {
                PrintError("Can't list data");
                return EXIT_FAILURE;
            }

            vector<Porto::Property> plist;
            ret = Api->Plist(plist);
            if (ret) {
                PrintError("Can't list properties");
                return EXIT_FAILURE;
            }

            for (const auto &arg : showData) {
                if (!ValidData(dlist, arg) && !ValidProperty(plist, arg)) {
                    TError error(EError::InvalidValue, "Invalid value");
                    PrintError(error, "Can't parse argument");
                    return EXIT_FAILURE;
                }
            }
        }

        string sortBy = showData[0];
        size_t nameLen = MaxFieldLength(clist, strlen("container"));

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

            fieldLen.push_back(MaxFieldLength(tmp));
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

class TCreateVolumeCmd final : public ICmd {
public:
    TCreateVolumeCmd(Porto::Connection *api) : ICmd(api, "vcreate", 1, "-A|<path> [property=value...]",
        "create volume",
        "    -A        choose path automatically\n"
        ) {}

    int Execute(TCommandEnviroment *env) final override {
        std::map<std::string, std::string> properties;
        const auto &args = env->GetArgs();
        std::string path = args[0];

        if (path == "-A") {
            path = "";
        } else {
            path = TPath(path).RealPath().ToString();
        }

        for (size_t i = 1; i < args.size(); i++) {
            const std::string &arg = args[i];
            std::size_t sep = arg.find('=');
            if (sep == string::npos)
                properties[arg] = "";
            else
                properties[arg.substr(0, sep)] = arg.substr(sep + 1);
        }

        Porto::Volume volume;
        int ret = Api->CreateVolume(path, properties, volume);
        if (ret) {
            PrintError("Can't create volume");
            return ret;
        }

        if (path == "")
            std::cout << volume.Path << std::endl;

        return 0;
    }
};

class TLinkVolumeCmd final : public ICmd {
public:
    TLinkVolumeCmd(Porto::Connection *api) : ICmd(api, "vlink", 1, "<path> [container]",
                    "link volume to container", "default container - current\n") {}

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        const auto path = TPath(args[0]).RealPath().ToString();
        int ret = Api->LinkVolume(path, (args.size() > 1) ? args[1] : "");
        if (ret)
            PrintError("Can't link volume");
        return ret;
    }
};

class TUnlinkVolumeCmd final : public ICmd {
public:
    TUnlinkVolumeCmd(Porto::Connection *api) : ICmd(api, "vunlink", 1,
                    "[-A] <path> [container]",
                    "unlink volume from container",
                    "    -A        unlink from all containers\n"
                    "default container - current\n"
                    "removing last link deletes volume\n") {}

    int Execute(TCommandEnviroment *env) final override {
        bool all = false;
        const auto &args = env->GetOpts({
            { 'A', false, [&](const char *arg) { all = true; } },
        });
        const auto path = TPath(args[0]).RealPath().ToString();
        std::vector<Porto::Volume> vol;
        int ret;

        if (all) {
            ret = Api->ListVolumes(path, "", vol);
            if (ret || vol.size() != 1) {
                PrintError("Cannot list volume");
                return EXIT_FAILURE;
            }

            for (auto ct: vol[0].Containers) {
                ret = Api->UnlinkVolume(path, ct);
                if (ret) {
                    PrintError("Cannot unlink volume from " + ct);
                    break;
                }
            }
        } else {
            ret = Api->UnlinkVolume(path, (args.size() > 1) ? args[1] : "");
            if (ret)
                PrintError("Cannot unlink volume");
        }
        return ret;
    }
};

class TListVolumesCmd final : public ICmd {
    bool details = true;
    bool verbose = false;
    bool inodes = false;

public:
    TListVolumesCmd(Porto::Connection *api) : ICmd(api, "vlist", 0, "[-1|-i|-v] [volume]...",
        "list volumes",
        "    -1        list only paths\n"
        "    -i        list inode information\n"
        "    -v        list all properties\n"
        ) {}

    void ShowSizeProperty(Porto::Volume &v, const char *p, int w, bool raw = false) {
      uint64_t val;

      if (!v.Properties.count(std::string(p)))
          std::cout << std::setw(w) << "-";
      else if (StringToUint64(v.Properties.at(std::string(p)), val))
          std::cout << std::setw(w) << "err";
      else if (raw)
          std::cout << std::setw(w) << val;
      else
          std::cout << std::setw(w) << StringFormatSize(val);
    }

    void ShowPercent(Porto::Volume &v, const char *u, const char *a, int w) {
      uint64_t used, avail;

      if (!v.Properties.count(std::string(u)) |
          !v.Properties.count(std::string(a)))
          std::cout << std::setw(w) << "-";
      else if (StringToUint64(v.Properties.at(std::string(u)), used) ||
               StringToUint64(v.Properties.at(std::string(a)), avail))
          std::cout << std::setw(w) << "err";
      else if (used + avail)
          std::cout << std::setw(w - 1) << std::llround(100. * used / (used + avail)) << "%";
      else
          std::cout << std::setw(w) << "inf";
    }

    void ShowVolume(Porto::Volume &v) {
        if (!details) {
            std::cout << v.Path << std::endl;
        } else {
            std::cout << std::left << std::setw(40) << v.Path << std::right;
            if (v.Path.length() > 40)
                std::cout << std::endl << std::setw(40) << " ";
            if (inodes) {
                ShowSizeProperty(v, V_INODE_LIMIT, 10, true);
                ShowSizeProperty(v, V_INODE_USED, 10, true);
                ShowSizeProperty(v, V_INODE_AVAILABLE, 10, true);
                ShowPercent(v, V_INODE_USED, V_INODE_AVAILABLE, 5);
            } else {
                ShowSizeProperty(v, V_SPACE_LIMIT, 10);
                ShowSizeProperty(v, V_SPACE_USED, 10);
                ShowSizeProperty(v, V_SPACE_AVAILABLE, 10);
                ShowPercent(v, V_SPACE_USED, V_SPACE_AVAILABLE, 5);
            }

            for (auto name: v.Containers)
                std::cout << " " << name;

            std::cout << std::endl;
        }

        if (!verbose)
            return;

        std::cout << "  " << std::left << std::setw(20) << "containers";
        for (auto name: v.Containers)
            std::cout << " " << name;
        std::cout << std::endl;
        for (auto kv: v.Properties) {
             std::cout << "  " << std::left << std::setw(20) << kv.first;
             if (kv.second.length())
                  std::cout << " " << kv.second;
             std::cout << std::endl;
        }
        std::cout << std::endl;
    }

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetOpts({
            { '1', false, [&](const char *arg) { details = false; } },
            { 'i', false, [&](const char *arg) { inodes = true; } },
            { 'v', false, [&](const char *arg) { verbose = true; details = false; } },
        });

        vector<Porto::Volume> vlist;

        if (details) {
            std::cout << std::left << std::setw(40) << "Volume" << std::right;
            std::cout << std::setw(10) << "Limit";
            std::cout << std::setw(10) << "Used";
            std::cout << std::setw(10) << "Avail";
            std::cout << std::setw(5) << "Use%";
            std::cout << std::left << " Containers" << std::endl;
        }

        if (args.empty()) {
          int ret = Api->ListVolumes(vlist);
          if (ret) {
              PrintError("Can't list volumes");
              return ret;
          }

          for (auto &v : vlist)
              ShowVolume(v);
        } else {
            for (const auto &arg : args) {
                const auto path = TPath(arg).RealPath().ToString();

                vlist.clear();
                int ret = Api->ListVolumes(path, "", vlist);
                if (ret) {
                    PrintError(arg);
                    continue;
                }
                for (auto &v : vlist)
                    ShowVolume(v);
            }
        }

        return EXIT_SUCCESS;
    }
};

class TTuneVolumeCmd final : public ICmd {
public:
    TTuneVolumeCmd(Porto::Connection *api) :
        ICmd(api, "vtune", 1, "<path> [property=value...]", "tune volume") { }

    int Execute(TCommandEnviroment *env) final override {
        std::map<std::string, std::string> properties;
        const auto &args = env->GetArgs();
        const auto path = TPath(args[0]).RealPath().ToString();

        for (size_t i = 1; i < args.size(); i++) {
            const std::string &arg = args[i];
            std::size_t sep = arg.find('=');
            if (sep == string::npos)
                properties[arg] = "";
            else
                properties[arg.substr(0, sep)] = arg.substr(sep + 1);
        }

        int ret = Api->TuneVolume(path, properties);
        if (ret) {
            PrintError("Cannot tune volume");
            return ret;
        }

        return 0;
    }
};

class TLayerCmd final : public ICmd {
public:
    TLayerCmd(Porto::Connection *api) : ICmd(api, "layer", 0,
        "-I|-M|-R|-L|-F|-E <layer> [tarball]",
        "Manage overlayfs layers in internal storage",
        "    -I <layer> <tarball>     import layer from tarball\n"
        "    -M <layer> <tarball>     merge tarball into existing or new layer\n"
        "    -R <layer> [layer...]    remove layer from storage\n"
        "    -F                       remove all unused layes\n"
        "    -L                       list present layers\n"
        "    -E <volume> <tarball>    export upper layer into tarball\n"
        ) {}

    bool import = false;
    bool merge  = false;
    bool remove = false;
    bool list   = false;
    bool export_ = false;
    bool flush = false;

    int Execute(TCommandEnviroment *env) final override {
        int ret = EXIT_SUCCESS;
        const auto &args = env->GetOpts({
            { 'I', false, [&](const char *arg) { import = true; } },
            { 'M', false, [&](const char *arg) { merge  = true; } },
            { 'R', false, [&](const char *arg) { remove = true; } },
            { 'F', false, [&](const char *arg) { flush  = true; } },
            { 'L', false, [&](const char *arg) { list   = true; } },
            { 'E', false, [&](const char *arg) { export_= true; } },
        });

        std::string path;
        if (args.size() >= 2)
            path = TPath(args[1]).AbsolutePath().ToString();

        if (import) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], path);
            if (ret)
                PrintError("Can't import layer");
        } else if (export_) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ExportLayer(args[0], path);
            if (ret)
                PrintError("Can't export layer");
        } else if (merge) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], path, true);
            if (ret)
                PrintError("Can't merge layer");
        } else if (remove) {
            if (args.size() < 1)
                return EXIT_FAILURE;

            for (const auto &arg : args) {
                ret = Api->RemoveLayer(arg);
                if (ret)
                    PrintError("Can't remove layer");
            }
        } else if (flush) {
            std::vector<std::string> layers;
            ret = Api->ListLayers(layers);
            if (ret) {
                PrintError("Can't list layers");
                return EXIT_FAILURE;
            } else {
                for (const auto &l: layers)
                    (void)Api->RemoveLayer(l);
            }
        } else if (list) {
            std::vector<std::string> layers;
            ret = Api->ListLayers(layers);
            if (ret) {
                PrintError("Can't list layers");
            } else {
                for (const auto &l: layers)
                    std::cout << l << std::endl;
            }
        } else
            return EXIT_FAILURE;

        return ret;
    }
};

class TBuildCmd final : public ICmd {
public:
    TBuildCmd(Porto::Connection *api) : ICmd(api, "build", 0,
            "[-k] [-M] [-l|-L layer]... [-o layer.tar] [-O image.img] [-B bootstrap] [-S script]... [properties]...",
            "build container image",
            "    -l layer|dir|tarball       layer for bootstrap, if empty run in host\n"
            "    -L layer|dir|tarball       add lower layer (-L top ... -L bottom)\n"
            "    -o layer.tar               save as overlayfs layer\n"
            "    -O image.img               save as filesystem image\n"
            "    -B bootstrap               bash script runs outside (with cwd=volume)\n"
            "    -S script                  bash script runs inside (with root=volume)\n"
            "    -M                         merge all layers together\n"
            "    -k                         keep volume and container\n"
            ) { }

    ~TBuildCmd() { }

    int Execute(TCommandEnviroment *environment) final override {
        TLauncher launcher(Api);
        TLauncher bootstrap(Api);
        TError error;

        launcher.Container = "portoctl-build-" + std::to_string(GetPid());
        launcher.SetProperty("net", "NAT");
        launcher.WeakContainer = true;
        launcher.NeedVolume = true;
        launcher.StartContainer = false;
        launcher.ChrootVolume = false;
        launcher.StartOS = true;

        TPath output;
        TPath outputImage;
        TPath loopStorage, loopImage;
        std::vector<std::string> env;
        std::vector<std::string> layers;
        std::vector<TPath> scripts;
        TPath bootstrap_script;

        const auto &opts = environment->GetOpts({
            { 'L', true, [&](const char *arg) { launcher.Layers.push_back(arg); } },
            { 'l', true, [&](const char *arg) { bootstrap.Layers.push_back(arg); bootstrap.NeedVolume = true; } },
            { 'o', true, [&](const char *arg) { output = TPath(arg).AbsolutePath(); } },
            { 'O', true, [&](const char *arg) { outputImage = TPath(arg).AbsolutePath(); } },
            { 'B', true, [&](const char *arg) { bootstrap_script = TPath(arg).RealPath(); } },
            { 'S', true, [&](const char *arg) { scripts.push_back(TPath(arg).RealPath()); } },
            { 'k', false, [&](const char *arg) { launcher.WeakContainer = false; } },
            { 'M', false, [&](const char *arg) { launcher.MergeLayers = true; } },
        });

        if (output.IsEmpty() && outputImage.IsEmpty()) {
            std::cerr << "No output file specified" << std::endl;
            PrintUsage();
            return EXIT_FAILURE;
        }

        if (!output.IsEmpty()) {
            if (output.Exists()) {
                std::cerr << "Output file " << output << " already exists" << std::endl;
                return EXIT_FAILURE;
            }
            if (!output.DirName().Exists()) {
                std::cerr << "Output directory " << output.DirName() << " not exists" << std::endl;
                return EXIT_FAILURE;
            }
        }

        if (!outputImage.IsEmpty()) {
            if (outputImage.Exists()) {
                std::cerr << "Output file " << outputImage << " already exists" << std::endl;
                return EXIT_FAILURE;
            }
            if (!outputImage.DirName().Exists()) {
                std::cerr << "Output directory " << outputImage.DirName() << " not exists" << std::endl;
                return EXIT_FAILURE;
            }
        }

        for (auto &script: scripts) {
            if (!script.Exists()) {
                std::cerr << "Script " << script << " not exists" << std::endl;
                return EXIT_FAILURE;
            }
        }

        if (!bootstrap_script.IsEmpty() && !bootstrap_script.Exists()) {
            std::cerr << "Bootstrap " << bootstrap_script << " not exists" << std::endl;
            return EXIT_FAILURE;
        }

        /* add properies from commandline */
        for (auto &arg: opts) {
            error = launcher.SetProperty(arg);
            if (error) {
                std::cerr << "Cannot set property: " << error << std::endl;
                return EXIT_FAILURE;
            }
        }

        if (!outputImage.IsEmpty()) {
            error = loopStorage.MkdirTmp(outputImage.DirName(), "loop.", 0755);
            if (error) {
                std::cerr << "Cannot create storage for loop: " << error << std::endl;
                return EXIT_FAILURE;
            }
            launcher.VolumeBackend = "loop";
            launcher.VolumeStorage = loopStorage.ToString();
            loopImage = loopStorage / "loop.img";
        }

        /* do not start os for bootstrap */
        bool StartOS = launcher.StartOS;
        launcher.StartOS = false;

        std::string volume;
        TPath volume_script;

        error = launcher.Launch();
        if (error) {
            std::cerr << "Cannot create volume: " << error << std::endl;
            goto err;
        }

        volume = launcher.Volume.Path;

        volume_script = TPath(volume + "/script");
        volume_script.Unlink();
        error = volume_script.Mkfile(0644);
        if (error) {
            std::cerr << "Cannot create script: " << error << std::endl;
            goto err;
        }

        if (!bootstrap_script.IsEmpty()) {
            bootstrap.Container = launcher.Container + "/bootstrap";
            bootstrap.ForwardStreams = true;
            bootstrap.WaitExit = true;
            bootstrap.StartOS = true;

            std::string script_text;
            error = bootstrap_script.ReadAll(script_text);
            if (!error)
                error = volume_script.WriteAll(script_text);
            if (error) {
                std::cout << "Cannot copy script: " << error << std::endl;
                goto err;
            }

            bootstrap.SetProperty("stdin_path", "/dev/null");
            bootstrap.SetProperty("isolate", "true");
            bootstrap.SetProperty("net", "inherited");

            /* give write access only to volume and /tmp */
            if (bootstrap.Layers.empty()) {
                bootstrap.SetProperty("bind", volume + " " + volume + " rw;/tmp /tmp rw");
                bootstrap.SetProperty("root_readonly" "true");
            } else {
                bootstrap.SetProperty("bind", volume + " " + volume + " rw");
            }

            bootstrap.SetProperty("cwd", volume);
            bootstrap.SetProperty("command", "/bin/bash -e -x -c '. ./script'");

            std::cout << "Starting bootstrap " << bootstrap_script << std::endl;

            error = bootstrap.Launch();
            if (error) {
                std::cout << "Cannot start bootstrap: " << error << std::endl;
                goto err;
            }

            if (bootstrap.ExitCode) {
                std::cout << "Bootstrap: " << bootstrap.ExitMessage << std::endl;
                goto err;
            }

            bootstrap.Cleanup();

            if (launcher.StopContainer())
                goto err;
        }

        launcher.ChrootVolume = true;
        launcher.StartOS = StartOS;

        error = launcher.ApplyConfig();
        if (error) {
            std::cout << "Cannot configure launcher: " << error << std::endl;
            goto err;
        }

        for (auto &script: scripts) {
            TLauncher executor(Api);

            std::string script_text;
            error = script.ReadAll(script_text);
            if (!error)
                error = volume_script.WriteAll(script_text);
            if (error) {
                std::cout << "Cannot copy script: " << error << std::endl;
                goto err;
            }

            executor.Container = launcher.Container + "/script";
            executor.ForwardStreams = true;
            executor.WaitExit = true;

            executor.SetProperty("stdin_path", "/dev/null");
            executor.SetProperty("isolate", "false");
            executor.SetProperty("virt_mode", "os");
            executor.SetProperty("net", "inherited");
            executor.SetProperty("command", "/bin/bash -e -x -c '. ./script'");

            std::cout << "Starting script " << script << std::endl;

            error = executor.Launch();
            if (error) {
                std::cout << "Cannot start script: " << error << std::endl;
                goto err;
            }

            if (executor.ExitCode) {
                std::cout << "Script: " << executor.ExitMessage << std::endl;
                goto err;
            }

            executor.Cleanup();
            volume_script.WriteAll("");
        }

        if (!scripts.empty() && launcher.StopContainer())
            goto err;

        volume_script.Unlink();

        if (!output.IsEmpty()) {
            std::cout << "Exporting layer into " << output.ToString() << std::endl;

            if (Api->ExportLayer(volume, output.ToString())) {
                std::cerr << "Cannot export layer:" << launcher.GetLastError() << std::endl;
                goto err;
            }
        }

        if (launcher.WeakContainer)
            launcher.Cleanup();

        if (!outputImage.IsEmpty()) {
            std::cout << "Exporting image into " << outputImage.ToString() << std::endl;

            error = loopImage.Rename(outputImage);
            if (error) {
                std::cerr << "Cannot export image:" << error << std::endl;
                goto err;
            }
            (void)loopStorage.Rmdir();
        }

        return EXIT_SUCCESS;

err:
        if (launcher.WeakContainer)
            launcher.Cleanup();
        if (!loopImage.IsEmpty()) {
            (void)loopImage.Unlink();
            (void)loopStorage.Rmdir();
        }
        return EXIT_FAILURE;
    }
};

class TConvertPathCmd final : public ICmd {

public:
    TConvertPathCmd(Porto::Connection *api) : ICmd(api, "convert", 1,
                                           "<path> [-s container] [-d container]",
                                           "convert paths between different containers",
                                           "    -s container    source container (client container if omitted)\n"
                                           "    -d container    destination container (client container if omitted)\n") { }

    int Execute(TCommandEnviroment *environment) final override {
        std::string path, src, dest;
        environment->GetOpts({
                {'s', true, [&](const char *arg) { src = arg; }},
                {'d', true, [&](const char *arg) { dest = arg; }}
	  });
        path = environment->GetArgs()[0];

        std::string converted;
        auto ret = Api->ConvertPath(path, src, dest, converted);
        if (ret)
            PrintError("Can't convert path");
        else
            std::cout << converted << std::endl;
        return ret;
    }
};

int main(int argc, char *argv[]) {
    Porto::Connection api;
    TCommandHandler handler(api);

    handler.RegisterCommand<TCreateCmd>();
    handler.RegisterCommand<TDestroyCmd>();
    handler.RegisterCommand<TListCmd>();
    handler.RegisterCommand<TTopCmd>();
    handler.RegisterCommand<TSortCmd>();
    handler.RegisterCommand<TStartCmd>();
    handler.RegisterCommand<TStopCmd>();
    handler.RegisterCommand<TRestartCmd>();
    handler.RegisterCommand<TKillCmd>();
    handler.RegisterCommand<TPauseCmd>();
    handler.RegisterCommand<TResumeCmd>();
    handler.RegisterCommand<TGetPropertyCmd>();
    handler.RegisterCommand<TSetPropertyCmd>();
    handler.RegisterCommand<TGetDataCmd>();
    handler.RegisterCommand<TGetCmd>();
    handler.RegisterCommand<TRawCmd>();
    handler.RegisterCommand<TEnterCmd>();
    handler.RegisterCommand<TRunCmd>();
    handler.RegisterCommand<TExecCmd>();
    handler.RegisterCommand<TShellCmd>();
    handler.RegisterCommand<TGcCmd>();
    handler.RegisterCommand<TFindCmd>();
    handler.RegisterCommand<TWaitCmd>();

    handler.RegisterCommand<TCreateVolumeCmd>();
    handler.RegisterCommand<TLinkVolumeCmd>();
    handler.RegisterCommand<TUnlinkVolumeCmd>();
    handler.RegisterCommand<TListVolumesCmd>();
    handler.RegisterCommand<TTuneVolumeCmd>();

    handler.RegisterCommand<TLayerCmd>();
    handler.RegisterCommand<TBuildCmd>();

    handler.RegisterCommand<TConvertPathCmd>();

    TLogger::DisableLog();

    int ret = handler.HandleCommand(argc, argv);
    if (ret < 0) {
        return EXIT_FAILURE;
    } else {
        return ret;
    }
};
