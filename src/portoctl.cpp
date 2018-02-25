#include <iostream> // FIXME KILL THIS SHIT
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

static void ForwardWinch(int) {
    struct winsize winsize;

    /* Copy window size into master terminal */
    if (!ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize))
        (void)ioctl(ForwardPtyMaster, TIOCSWINSZ, &winsize);
}

volatile int ChildDead;

static void CatchChild(int) {
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

    std::string Private;

    Porto::Volume Volume;
    std::string SpaceLimit;
    std::string VolumeBackend;
    std::string VolumeStorage;
    std::string Place;
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
            auto list = SplitEscapedString(val, ';');
            Environment.insert(Environment.end(), list.begin(), list.end());
        } else if (key == "space_limit") {
            SpaceLimit = val;
            NeedVolume = true;
        } else if (key == "backend") {
            VolumeBackend = val;
            NeedVolume = true;
        } else if (key == "storage") {
            NeedVolume = true;
            VolumeStorage = val;
        } else if (key == "layers") {
            NeedVolume = true;
            Layers = SplitEscapedString(val, ';');
        } else if (key == "place") {
            Place = val;
            Properties.emplace_back(key, val);
        } else if (key == "private") {
            Private = val;
            Properties.emplace_back(key, val);
        } else
            Properties.emplace_back(key, val);

        return OK;
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
        std::cout << "Importing layer " << path << " as " << id << std::endl;
        if (Api->ImportLayer(id, path.ToString(), false, Place))
            return GetLastError();
        ImportedLayers.push_back(id);
        return OK;
    }

    TError ImportLayers() {
        std::vector<Porto::Layer> known;
        TError error;

        if (Api->ListLayers(known, Place))
            return GetLastError();

        for (auto &layer : Layers) {
            bool found = false;
            for (auto &l: known) {
                if (l.Name == layer) {
                    found = true;
                    break;
                }
            }
            if (found) {
                VolumeLayers.push_back(layer);
                continue;
            }

            auto path = TPath(layer).RealPath();

            if (path.IsDirectoryFollow() || VolumeBackend == "squash") {
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
            config["layers"] = MergeEscapeStrings(VolumeLayers, ';');
        }

        if (VolumeBackend != "")
            config["backend"] = VolumeBackend;
        else if (MergeLayers || Layers.empty())
            config["backend"] = "native";
        else
            config["backend"] = "overlay";

        if (VolumeStorage != "")
            config["storage"] = VolumeStorage;

        if (Place != "")
            config["place"] = Place;

        if (Private != "")
            config["private"] = Private;

        if (Api->CreateVolume("", config, Volume))
            return GetLastError();
        VolumeLinked = true;

        if (Container != "") {
            if (Api->LinkVolume(Volume.Path, Container))
                return GetLastError();
            if (Api->UnlinkVolume(Volume.Path))
                return GetLastError();
            VolumeLinked = false;
        }

        return OK;
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

        if (Api->GetProperty(Container, "exit_status", result))
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
            return TError::System("Cannot open master terminal");

        char slave[128];
        if (ptsname_r(MasterPty, slave, sizeof(slave)))
            return TError::System("Cannot get terminal name");

        if (unlockpt(MasterPty))
            return TError::System("Cannot unlock terminal");

        SlavePty = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (SlavePty < 0)
            return TError::System("Cannot open slave terminal");

        return OK;
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
            return TError::System("cannot fork");

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

            if (kill(pid, SIGKILL))
                std::cerr << "Cannot kill portoctl tty child : " << GetLastError() << std::endl;
        }

        /* restore state of outer terminal */
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &termios);

        Signal(SIGWINCH, SIG_DFL);
        Signal(SIGCHLD, SIG_DFL);

        CloseMasterPty();

        return OK;
    }

    TError ApplyConfig() {

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

        if (Api->SetProperty(Container, "virt_mode", StartOS ? "os" : "app"))
            goto err;

        if (Api->SetProperty(Container, "env", MergeEscapeStrings(Environment, ';')))
            goto err;

        return OK;
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

        /* forward terminal only if stdin is a tty */
        if (ForwardTerminal)
             ForwardTerminal = isatty(STDIN_FILENO);

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

        return OK;

err:
        Cleanup();
        return error;
    }

    TError StopContainer() {
        if (Api->Stop(Container)) {
            std::cerr << "Cannot stop container " << Container << " : " << GetLastError() << std::endl;
            return GetLastError();
        }
        return OK;
    }

    void Cleanup() {
        if (ContainerCreated) {
            if (Api->Destroy(Container))
                std::cerr << "Cannot destroy container " << Container << " : " << GetLastError() << std::endl;
            ContainerCreated = false;
        }

        if (VolumeLinked) {
            if (Api->UnlinkVolume(Volume.Path))
                std::cerr << "Cannot unlink volume " << Volume.Path << " : " << GetLastError() << std::endl;
            VolumeLinked = false;
        }

        for (auto &layer : ImportedLayers) {
            if (Api->RemoveLayer(layer, Place) && GetLastError() != EError::LayerNotFound)
                std::cerr << "Cannot remove layer " << layer << " : " << GetLastError() << std::endl;
        }
        ImportedLayers.clear();
        VolumeLayers.clear();
        CloseSlavePty();
        CloseMasterPty();
    }
};

static const std::string StripIdx(const std::string &name) {
    auto idx = name.find('[');
    if (idx != std::string::npos)
        return std::string(name.c_str(), idx);
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

static std::string HumanValue(const std::string &name, const std::string &val) {
    TUintMap map;
    uint64_t num;

    if (name == "stdout" || name == "stderr") {
        if (val.size() > 4096)
            return val.substr(0, 2048) + "\n... skip " +
                std::to_string(val.size() - 4096) + " bytes ...\n" +
                val.substr(val.size() - 2048);
        return val;
    }

    if (name == "env" ||
            name == "net" ||
            name == "ip" ||
            name == "default_gw" ||
            name == "devices" ||
            name == "bind" ||
            name == "ulimit" ||
            name == "cgroups" ||
            name == "controllers" ||
            name == "capabilities" ||
            name == "capabilities_ambient" ||
            name == "resolv_conf" ||
            name == "volumes_owned" ||
            name == "volumes_linked" ||
            name == "volumes_required" ||
            name == "net_drops" ||
            name == "net_overlimits" ||
            name == "net_packets" ||
            name == "net_rx_drops" ||
            name == "net_rx_packets" ||
            name == "net_tx_drops" ||
            name == "net_tx_packets" ||
            name == "net_class_id" ||
            name == "io_ops_limit" ||
            name == "io_ops")
        return StringReplaceAll(val, ";", ";\n      ");

    if (name == "net_limit" ||
         name == "net_guarantee" ||
         name == "net_bytes" ||
         name == "net_rx_bytes" ||
         name == "net_tx_bytes" ||
         name == "place_usage" ||
         name == "place_limit" ||
         name == "io_limit" ||
         name == "io_read" ||
         name == "io_write") {
        if (!StringToUintMap(val, map)) {
            std::stringstream str;
            for (auto kv : map) {
                if (str.str().length())
                    str << ";\n       ";
                str << kv.first << ": " << StringFormatSize(kv.second);
            }
            return str.str();
        } else
            return StringReplaceAll(val, ";", ";\n      ");
    }

    if (name == "io_time") {
        if (!StringToUintMap(val, map)) {
            std::stringstream str;
            for (auto kv : map) {
                if (str.str().length())
                    str << ";\n       ";
                str << kv.first << ": " << StringFormatDuration(kv.second / 1000000);
            }
            return str.str();
        } else
            return StringReplaceAll(val, ";", ";\n      ");
    }

    if (val == "" || StringToUint64(val, num))
        return val;

    if (name == "memory_guarantee" ||
        name == "memory_limit" ||
        name == "memory_usage" ||
        name == "memory_limit_total" ||
        name == "memory_guarantee_total" ||
        name == "anon_usage" ||
        name == "cache_usage" ||
        name == "anon_limit" ||
        name == "max_rss" ||
        name == "stdout_limit" ||
        name == "hugetlb_limit" ||
        name == "hugetlb_usage")
        return StringFormatSize(num);

    if (name == "time" || name == "aging_time")
        return StringFormatDuration(num * 1000);

    if (name == "cpu_usage" || name == "cpu_usage_system" ||  name == "cpu_wait")
        return StringFormatDuration(num / 1000000);

    if (name == "exit_status")
        return FormatExitStatus(num);

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
    TGetPropertyCmd(Porto::Connection *api) : ICmd(api, "pget", 2,
            "[-s] [-k] <container> <property> [property...]",
            "get raw container property") {}

    int Execute(TCommandEnviroment *env) final override {
        bool printKey = false;
        int flags = 0;

        const auto &args = env->GetOpts({
            { 'k', false, [&](const char *) { printKey = true; } },
            { 's', false, [&](const char *) { flags |= Porto::GetFlags::Sync; } },
        });

        for (size_t i = 1; i < args.size(); ++i) {
            string value;
            int ret = Api->GetProperty(args[0], args[i], value, flags);
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
    TGetDataCmd(Porto::Connection *api) : ICmd(api, "dget", 2,
            "[-s] [-k] <container> <property>...",
            "get raw container property") {}

    int Execute(TCommandEnviroment *env) final override {
        bool printKey = false;
        int flags = 0;

        const auto &args = env->GetOpts({
            { 'k', false, [&](const char *) { printKey = true; } },
            { 's', false, [&](const char *) { flags |= Porto::GetFlags::Sync; } },
        });

        for (size_t i = 1; i < args.size(); ++i) {
            string value;
            int ret = Api->GetProperty(args[0], args[i], value, flags);
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
    { "SIGUNUSED",  SIGSYS },
};

class TKillCmd final : public ICmd {
public:
    TKillCmd(Porto::Connection *api) : ICmd(api, "kill", 1, "<container> [signal]", "send signal to container") {}

    int Execute(TCommandEnviroment *env) final override {
        int sig = SIGTERM;
        const auto &args = env->GetArgs();
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

class TRespawnCmd final : public ICmd {
public:
    TRespawnCmd(Porto::Connection *api) : ICmd(api, "respawn", 1, "<container1> [container2...]", "respawn container") {}
    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Respawn(arg);
            if (ret) {
                PrintError("Cannot respawn container");
                return ret;
            }
        }
        return 0;
    }
};

class TGetCmd final : public ICmd {
public:
    TGetCmd(Porto::Connection *api) : ICmd(api, "get", 1,
            "[-n] [-s] [-r] [container|pattern]... [--] [property]...",
            "get container properties",
            "   -n   non-blocking\n"
            "   -s   synchronize cached values\n"
            "   -r   only real values\n") {}

    int Execute(TCommandEnviroment *env) final override {
        bool printKey = true;
        bool printErrors = false;
        bool printEmpty = false;
        bool printHuman = true;
        bool multiGet = false;
        int flags = 0;
        std::vector<std::string> list;
        std::vector<std::string> vars;
        int ret;

        const auto &args = env->GetOpts({
                {'n', false, [&](const char *) { flags |= Porto::GetFlags::NonBlock; }},
                {'s', false, [&](const char *) { flags |= Porto::GetFlags::Sync; }},
                {'r', false, [&](const char *) { flags |= Porto::GetFlags::Real; }},
        });

        int sep = -1;
        for (auto i = 0u; i < args.size(); i++) {
            if (args[i] == "--") {
                sep = i;
                break;
            }
        }

        if (sep == 0) {
            list.push_back("***");
            vars.insert(vars.end(), args.begin() + 1, args.end());
        } else if (sep > 0) {
            list.insert(list.end(), args.begin(), args.begin() + sep);
            vars.insert(vars.end(), args.begin() + sep + 1, args.end());
        } else {
            list.push_back(args[0]);
            vars.insert(vars.end(), args.begin() + 1, args.end());
        }

        if (vars.size()) {
            if (vars.size() == 1)
                printKey = false;
            printErrors = true;
            printEmpty = true;
            printHuman = false;
        } else {
            vector<Porto::Property> plist;
            ret = Api->ListProperties(plist);
            if (ret) {
                PrintError("Can't list properties");
                return EXIT_FAILURE;
            }
            vars.reserve(plist.size());
            for (auto p : plist)
                vars.push_back(p.Name);
            std::sort(vars.begin(), vars.end());
        }

        std::map<std::string, std::map<std::string, Porto::GetResponse>> result;
        ret = Api->Get(list, vars, result, flags);
        if (ret) {
            PrintError("Can't get containers' data");
            return ret;
        }

        if (result.size() > 1)
            multiGet = true;

        ret = EXIT_SUCCESS;

        for (auto &it: result) {
            auto &data = it.second;

            if (multiGet)
                Print(it.first + ":");

            for (const auto &key : vars) {
                if (data[key].Error) {
                    if (printErrors || key == "state") {
                        TError error((rpc::EError)data[key].Error, data[key].ErrorMsg);
                        PrintError(error, "Can't get " + key);
                        ret = EXIT_FAILURE;
                    }
                    continue;
                }

                auto val = data[key].Value;
                if (val.empty() && !printEmpty)
                continue;

                if (printHuman)
                    val = HumanValue(key, val);

                if (printKey)
                    PrintPair(key, val);
                else
                    Print(val);
            }

            if (multiGet)
                Print("");
        }

        return ret;
    }
};

class TRunCmd final : public ICmd {
public:
    TRunCmd(Porto::Connection *api) : ICmd(api, "run", 2,
            "[-L layer] [-W] ... <container> [properties]",
            "create and start container with given properties",
            "    -L layer|dir|tarball        add lower layer (-L top ... -L bottom)\n"
            "    -W                          wait until container exits\n")
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
            { 'W', false, [&](const char *) {
                    launcher.WaitExit = true;
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
            { 'C', false, [&](const char *) { launcher.WeakContainer = false; } },
            { 'T', false, [&](const char *) { launcher.ForwardTerminal = false; } },
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

        bool command_found = false;
        for (auto &kv : launcher.Properties) {
            if (kv.first == "command") {
                command_found = true;

                if (StringTrim(kv.second).empty()) {
                    std::cerr << "Exec with empty command is not supported"
                              << std::endl;
                    return EXIT_FAILURE;
                }
            }
        }

        if (!command_found) {
            std::cerr << "Meta container exec is not supported, "
                         "please supply command property" << std::endl;
            return EXIT_FAILURE;
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

        auto ct = args[0];
        if (args.size() > 1) {
            command = args[1];
            for (size_t i = 2; i < args.size(); ++i)
                command += " " + args[i];
        }

        launcher.WeakContainer = true;
        launcher.ForwardTerminal = true;
        launcher.ForwardStreams = true;
        launcher.WaitExit = true;

        launcher.Container = ct + "/shell-" + current_user + "-" + std::to_string(GetPid());

        int name_max = getuid() ? CONTAINER_PATH_MAX_FOR_SUPERUSER : CONTAINER_PATH_MAX;
        if (launcher.Container.size() > name_max)
            launcher.Container = ct + "/shell-"+ std::to_string(GetPid());

        launcher.SetProperty("command", command);
        launcher.SetProperty("isolate", "false");
        launcher.SetProperty("private", "portoctl shell by " + current_user);

        if (ct.size() <= 50)
            launcher.Environment.push_back("debian_chroot=" + ct);
        else
            launcher.Environment.push_back("debian_chroot=" +
                    ct.substr(0, 24) + ".." + ct.substr(ct.size() - 24));

        launcher.Environment.push_back("PORTO_SHELL_NAME=" + ct);
        launcher.Environment.push_back("PORTO_SHELL_USER=" + current_user);

        if (user != "") {
            if (user == "root") {
                launcher.SetProperty("virt_mode", "os");
                launcher.SetProperty("net", "inherited");
            } else
                launcher.SetProperty("user", user);

            if (group == "") {
                TCred cred;
                if (!cred.Init(user))
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

    int Execute(TCommandEnviroment *) final override {
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
            ret = Api->GetProperty(c, "state", state);
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
    TFindCmd(Porto::Connection *api) : ICmd(api, "find", 1, "<pid> [comm]", "find container for given process id") {}

    int Execute(TCommandEnviroment *env) final override {
        int pid;
        const auto &args = env->GetArgs();
        TError error = StringToInt(args[0], pid);
        std::string comm;
        if (error) {
            std::cerr << "Can't parse pid " << args[0] << std::endl;
            return EXIT_FAILURE;
        }

        if (args.size() > 1)
            comm = args[1];
        else
            comm = GetTaskName(pid);

        std::string name;

        int ret = Api->LocateProcess(pid, comm, name);

        if (ret) {
            PrintError("Cannot find container by pid");
            return ret;
        }

        Print(name);

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
    TListCmd(Porto::Connection *api) : ICmd(api, "list", 0,
            "[-1] [-f] [-r] [-t] [pattern]",
            "list containers",
            "    -1        only names\n"
            "    -f        forest\n"
            "    -r        only running\n"
            "    -t        only toplevel\n"
            "\n"
            "patterns:\n"
            " \"***\"        all containres\n"
            " \"*\"          all first level\n"
            " \"foo*/bar*\"  all matching\n"
            "\n"
            ) {}

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
        bool running = false;
        const auto &args = env->GetOpts({
            { '1', false, [&](const char *) { details = false; } },
            { 'f', false, [&](const char *) { forest = true; } },
            { 't', false, [&](const char *) { toplevel = true; } },
            { 'r', false, [&](const char *) { running = true; } },
        });
        std::string mask = args.size() ? args[0] : "";

        vector<string> clist;
        int ret = Api->List(clist, mask);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        if (clist.empty())
            return EXIT_SUCCESS;

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

            if (running && state.Value != "running")
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

extern int portotop(Porto::Connection *api, const std::vector<std::string> &args);
class TTopCmd final : public ICmd {
public:
    TTopCmd(Porto::Connection *api) : ICmd(api, "top", 0, "[config]", "top-like tool for container monitoring and control") {}

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        return portotop(Api, args);
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
            vector<Porto::Property> plist;
            ret = Api->ListProperties(plist);
            if (ret) {
                PrintError("Can't list properties");
                return EXIT_FAILURE;
            }

            for (const auto &arg : showData) {
                if (!ValidProperty(plist, arg)) {
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
            ret = Api->GetProperty(container, "state", state);
            if (ret) {
                PrintError("Can't get container state");
                return EXIT_FAILURE;
            }

            if (state != "running" && state != "dead")
                continue;

            map<string, string> dataVal;
            for (auto data : showData) {
                string val;
                if (Api->GetProperty(container, data, val))
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
        std::cout << std::resetiosflags(std::ios::adjustfield);
        for (size_t i = 0; i < showData.size(); i++)
            std::cout << std::right << std::setw(fieldLen[i]) << showData[i];
        std::cout << std::endl;

        for (auto &pair : containerData) {
            std::cout << std::left << std::setw(nameLen) << pair.first;
            std::cout << std::resetiosflags(std::ios::adjustfield);

            for (size_t i = 0; i < showData.size(); i++) {
                std::cout << std::right << std::setw(fieldLen[i]);
                std::cout << HumanValue(showData[i], pair.second[showData[i]]);
                std::cout << std::resetiosflags(std::ios::adjustfield);
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
    TLinkVolumeCmd(Porto::Connection *api) : ICmd(api, "vlink", 1, "[-rR] <path> [container] [target]",
                    "link volume to container", "default container - current\n") {}

    int Execute(TCommandEnviroment *env) final override {
        bool required = false;
        bool read_only = false;
        const auto &args = env->GetOpts({
                {'r', false, [&](const char *) { read_only = true; }},
                {'R', false, [&](const char *) { required = true; }},
        });
        const auto path = TPath(args[0]).RealPath().ToString();
        int ret = Api->LinkVolume(path, (args.size() > 1) ? args[1] : "", (args.size() > 2) ? args[2] : "", read_only, required);
        if (ret)
            PrintError("Can't link volume");
        return ret;
    }
};

class TUnlinkVolumeCmd final : public ICmd {
public:
    TUnlinkVolumeCmd(Porto::Connection *api) : ICmd(api, "vunlink", 1,
                    "[-A] [-S] <path> [<container>|self|***] [<target>|***]",
                    "unlink volume from container",
                    "    -A        unlink from all containers, same as ***\n"
                    "    -S        strict unlink with non-lazy umount\n"
                    "default container - current, *** - unlink from all containers\n"
                    "default target - *** - unlink all targets\n"
                    "removing last link destroys volume\n") {}

    int Execute(TCommandEnviroment *env) final override {
        bool all = false;
        bool strict = false;
        const auto &args = env->GetOpts({
            { 'A', false, [&](const char *) { all = true; } },
            { 'S', false, [&](const char *) { strict = true; } },
        });
        const auto path = TPath(args[0]).RealPath().ToString();
        std::vector<Porto::Volume> vol;
        int ret;

        ret = Api->UnlinkVolume(path, (args.size() > 1) ? args[1] : all ? "***" : "",
                                args.size() > 2 ? args[2] : "***", strict);
        if (ret)
            PrintError("Cannot unlink volume");
        return ret;
    }
};

class TListVolumesCmd final : public ICmd {
    bool details = true;
    bool verbose = false;
    bool inodes = false;

public:
    TListVolumesCmd(Porto::Connection *api) : ICmd(api, "vlist", 0, "[-1|-i|-v] [-c <ct>] [volume]...",
        "list volumes",
        "    -1        list only paths\n"
        "    -i        list inode information\n"
        "    -v        list all properties\n"
        "    -c <ct>   list volumes linked to container\n"
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

            for (auto link: v.Links)
                std::cout << " " << link.Container;

            std::cout << std::endl;
        }

        if (!verbose)
            return;

        std::cout << "  " << std::left << std::setw(20) << "containers";
        for (auto link: v.Links)
            std::cout << " " << link.Container;
        std::cout << std::endl;

        for (auto link: v.Links)
            if (link.Target != "")
                std::cout << "  " << std::left << std::setw(20) << (link.ReadOnly ? "target_ro" : "target") << " " << link.Container << " " << link.Target << std::endl;

        std::cout << std::resetiosflags(std::ios::adjustfield);

        for (auto kv: v.Properties) {
             std::cout << "  " << std::left << std::setw(20) << kv.first;
             if (kv.second.length())
                  std::cout << " " << kv.second;

             std::cout << std::resetiosflags(std::ios::adjustfield);
             std::cout << std::endl;
        }
        std::cout << std::endl;
    }

    int Execute(TCommandEnviroment *env) final override {
        std::string container;
        const auto &args = env->GetOpts({
            { '1', false, [&](const char *) { details = false; } },
            { 'i', false, [&](const char *) { inodes = true; } },
            { 'v', false, [&](const char *) { verbose = true; details = false; } },
            { 'c', true , [&](const char *arg) { container = arg; } },
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
          int ret = Api->ListVolumes("", container, vlist);
          if (ret) {
              PrintError("Can't list volumes");
              return ret;
          }

          for (auto &v : vlist)
              ShowVolume(v);
        } else {
            int errors = 0;

            for (const auto &arg : args) {
                const auto path = TPath(arg).RealPath().ToString();

                vlist.clear();
                int ret = Api->ListVolumes(path, "", vlist);
                if (ret) {
                    PrintError(arg);
                    errors++;
                    continue;
                }
                for (auto &v : vlist)
                    ShowVolume(v);
            }

            if (errors)
                return EXIT_FAILURE;
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

class TStorageCmd final : public ICmd {
public:
    TStorageCmd(Porto::Connection *api) : ICmd(api, "storage", 0,
        "[-P <place>] -L|-R|-F <storage>",
        "Manage internal persistent volume storage",
        "    -P <place>               optional path to place\n"
        "    -L                       list existing storages\n"
        "    -R <storage>             remove storage\n"
        "    -F [days]                remove all unused for [days]\n"
        "    -S <private>             set private value\n"
        "    -I <storage> <archive>   import storage from archive\n"
        "    -E <storage> <archive>   export storage to archive\n"
        "    -c <compression>         override compression\n"
        ) {}

    int ret = EXIT_SUCCESS;
    bool list = false;
    bool remove = false;
    bool flush = false;
    bool import = false;
    bool export_ = false;
    std::string place;
    std::string private_;
    std::string compression;

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetOpts({
            { 'P', true,  [&](const char *arg) { place = arg;   } },
            { 'S', true,  [&](const char *arg) { private_ = arg;   } },
            { 'R', false, [&](const char *) { remove = true; } },
            { 'L', false, [&](const char *) { list = true;   } },
            { 'F', false, [&](const char *) { flush = true;   } },
            { 'I', false, [&](const char *) { import = true;   } },
            { 'E', false, [&](const char *) { export_ = true;   } },
            { 'c', true, [&](const char * arg) { compression = arg; } },
        });

        std::string archive;
        if (args.size() >= 2)
            archive = TPath(args[1]).AbsolutePath().ToString();

        std::string storage;
        if (remove) {
            if (args.size() < 1)
                return EXIT_FAILURE;

            ret = Api->RemoveStorage(args[0], place);
            if (ret)
                PrintError("Cannot remove storage");
        } else if (flush) {
            uint64_t age = 0;
            if (args.size() >= 1) {
                if (StringToUint64(args[0], age))
                    return EXIT_FAILURE;
                age *= 60*60*24;
            }
            std::vector<Porto::Storage> storage;
            ret = Api->ListStorage(storage, place);
            if (ret) {
                PrintError("Cannot list storage paths");
                return EXIT_FAILURE;
            }
            for (const auto &s: storage) {
                if (s.LastUsage < age)
                    continue;
                std::cout << "remove " << s.Name << std::endl;
                ret = Api->RemoveStorage(s.Name, place);
                if (ret)
                    PrintError("Cannot remove storage");
            }
        } else if (list) {
            std::vector<Porto::Storage> storage;
            ret = Api->ListStorage(storage, place);
            if (ret) {
                PrintError("Cannot list storage paths");
            } else {
                for (const auto &s: storage) {
                    std::cout << s.Name << std::endl;
                    if (s.OwnerUser.size())
                        std::cout << "\towner\t" << s.OwnerUser << ":" << s.OwnerGroup << std::endl;
                    if (s.LastUsage)
                        std::cout << "\tusage\t" << StringFormatDuration(s.LastUsage * 1000) << " ago" << std::endl;
                    if (s.PrivateValue.size())
                        std::cout << "\tprivate\t" << s.PrivateValue << std::endl;
                    std::cout << std::endl;
                }
            }
        } else if (import) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportStorage(args[0], archive, place, compression, private_);
            if (ret)
                PrintError("Cannot import storage");
        } else if (export_) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ExportStorage(args[0], archive, place, compression);
            if (ret)
                PrintError("Cannot export storage");
        } else {
            PrintUsage();
            return EXIT_FAILURE;
        }

        return ret;
    }
};

class TLayerCmd final : public ICmd {
public:
    TLayerCmd(Porto::Connection *api) : ICmd(api, "layer", 0,
        "[-P <place>] [-S <private>] -I|-M|-R|-L|-F|-E|-G <layer> [tarball]",
        "Manage overlayfs layers in internal storage",
        "    -P <place>               optional path to place\n"
        "    -S <private>             store layer private value while importing or separately\n"
        "    -I <layer> <tarball>     import layer from tarball\n"
        "    -M <layer> <tarball>     merge tarball into existing or new layer\n"
        "    -R <layer> [layer...]    remove layer from storage\n"
        "    -F [days]                remove all unused layers (unused for [days])\n"
        "    -L                       list present layers\n"
        "    -E <volume> <tarball>    export upper layer into tarball\n"
        "    -Q <volume> <squashfs>   export upper layer into squashfs\n"
        "    -c compression           override compression\n"
        "    -G <layer>               retrieve layer stored private value\n"
        "    -v                       be verbose\n"
        ) {}

    bool import = false;
    bool merge  = false;
    bool remove = false;
    bool list   = false;
    bool verbose = false;
    bool export_ = false;
    bool squash = false;
    bool flush = false;
    bool get_private = false;
    bool set_private = false;
    std::string place;
    std::string private_value;
    std::string compression;

    int Execute(TCommandEnviroment *env) final override {
        int ret = EXIT_SUCCESS;
        const auto &args = env->GetOpts({
            { 'P', true,  [&](const char *arg) { place = arg;   } },
            { 'I', false, [&](const char *) { import = true; } },
            { 'M', false, [&](const char *) { merge  = true; } },
            { 'R', false, [&](const char *) { remove = true; } },
            { 'F', false, [&](const char *) { flush  = true; } },
            { 'L', false, [&](const char *) { list   = true; } },
            { 'E', false, [&](const char *) { export_= true; } },
            { 'Q', false, [&](const char *) { squash = true; } },
            { 'G', false, [&](const char *) { get_private = true; } },
            { 'S', true, [&](const char *arg) { set_private = true; private_value = arg; } },
            { 'c', true, [&](const char *arg) { compression = arg; } },
            { 'v', false, [&](const char *) { verbose = true; } },
        });

        if (squash && compression.empty())
            compression = "squashfs";

        std::string path;
        if (args.size() >= 2)
            path = TPath(args[1]).AbsolutePath().ToString();

        if (import) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], path, false, place, private_value);
            if (ret)
                PrintError("Can't import layer");
        } else if (export_) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ExportLayer(args[0], path, compression);
            if (ret)
                PrintError("Can't export layer");
        } else if (squash) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ExportLayer(args[0], path, compression);
            if (ret)
                PrintError("Can't export layer");
        } else if (merge) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], path, true, place, private_value);
            if (ret)
                PrintError("Can't merge layer");
        } else if (remove) {
            if (args.size() < 1)
                return EXIT_FAILURE;

            for (const auto &arg : args) {
                ret = Api->RemoveLayer(arg, place);
                if (ret)
                    PrintError("Can't remove layer");
            }
        } else if (flush) {
            uint64_t age = 0;
            if (args.size() >= 1) {
                if (StringToUint64(args[0], age))
                    return EXIT_FAILURE;
                age *= 60*60*24;
            }
            std::vector<Porto::Layer> layers;
            ret = Api->ListLayers(layers, place);
            if (ret) {
                PrintError("Can't list layers");
                return EXIT_FAILURE;
            } else {
                for (const auto &l: layers) {
                    if (l.LastUsage < age)
                        continue;
                    if (verbose)
                        std::cout << "remove " << l.Name << std::endl;
                    ret = Api->RemoveLayer(l.Name, place);
                    if (ret)
                        PrintError("Cannot remove layer");
                }
            }
        } else if (list) {
            std::vector<Porto::Layer> layers;
            ret = Api->ListLayers(layers, place);
            if (ret) {
                PrintError("Can't list layers");
            } else {
                for (const auto &l: layers) {
                    std::cout << l.Name << std::endl;
                    if (!verbose)
                        continue;
                    if (l.OwnerUser.size())
                        std::cout << "\towner\t" << l.OwnerUser << ":" << l.OwnerGroup << std::endl;
                    if (l.LastUsage)
                        std::cout << "\tused\t" << StringFormatDuration(l.LastUsage * 1000) << " ago" << std::endl;
                    if (l.PrivateValue.size())
                        std::cout << "\tprivate\t" << l.PrivateValue << std::endl;
                    std::cout << std::endl;
                }
            }
        } else if (get_private) {
            if (args.size() < 1)
                return EXIT_FAILURE;
            ret = Api->GetLayerPrivate(private_value, args[0], place);

            if (ret)
                PrintError("Can't get layer private value");
            else
                std::cout << private_value << std::endl;
        } else if (set_private) {
            if (args.size() < 1)
                return EXIT_FAILURE;
            ret = Api->SetLayerPrivate(private_value, args[0], place);
            if (ret)
                PrintError("Can't set layer private value");
        } else {
            PrintUsage();
            return EXIT_FAILURE;
        }

        return ret;
    }
};

class TBuildCmd final : public ICmd {
public:
    TBuildCmd(Porto::Connection *api) : ICmd(api, "build", 0,
            "[-k] [-M] [-l|-L layer]... [-o layer.tar] [-O image.img] [-Q image.squashfs] [-B|-b script] [-S script]... [properties]...",
            "build container image",
            "    -l layer|dir|tarball       layer for bootstrap, if empty run in host\n"
            "    -L layer|dir|tarball       add lower layer (-L top ... -L bottom)\n"
            "    -o layer.tar               save as overlayfs layer\n"
            "    -O image.img               save as filesystem image\n"
            "    -Q image.squashfs          save as squashfs image\n"
            "    -c compression             override compression\n"
            "    -B bootstrap               bash script runs outside (with cwd=volume)\n"
            "    -b bootstrap2              bash script runs inside before os (with root=volume)\n"
            "    -S script                  bash script runs inside (with root=volume)\n"
            "    -M                         merge all layers together\n"
            "    -k                         keep volume and container\n"
            ) { }

    ~TBuildCmd() { }

    int Execute(TCommandEnviroment *environment) final override {
        TLauncher launcher(Api);
        TLauncher bootstrap(Api);
        TLauncher bootstrap2(Api);
        TLauncher chroot(Api);
        TError error;

        launcher.Container = "portoctl-build-" + std::to_string(GetPid());
        launcher.SetProperty("isolate", "false");
        launcher.SetProperty("net", "NAT");
        launcher.WeakContainer = true;
        launcher.NeedVolume = true;
        launcher.ChrootVolume = false;
        launcher.StartOS = true;

        TPath output;
        TPath outputImage;
        bool squash = false;
        std::string compression;
        TPath loopStorage, loopImage;
        std::vector<std::string> env;
        std::vector<std::string> layers;
        std::vector<TPath> scripts;
        TPath bootstrap_script;
        TPath bootstrap2_script;

        const auto &opts = environment->GetOpts({
            { 'L', true, [&](const char *arg) { launcher.Layers.push_back(arg); } },
            { 'l', true, [&](const char *arg) { bootstrap.Layers.push_back(arg); bootstrap.NeedVolume = true; } },
            { 'o', true, [&](const char *arg) { output = TPath(arg).AbsolutePath(); } },
            { 'O', true, [&](const char *arg) { outputImage = TPath(arg).AbsolutePath(); } },
            { 'Q', true, [&](const char *arg) { outputImage = TPath(arg).AbsolutePath(); squash = true; } },
            { 'c', true, [&](const char *arg) { compression = arg; } },
            { 'B', true, [&](const char *arg) { bootstrap_script = TPath(arg).RealPath(); } },
            { 'b', true, [&](const char *arg) { bootstrap2_script = TPath(arg).RealPath(); } },
            { 'S', true, [&](const char *arg) { scripts.push_back(TPath(arg).RealPath()); } },
            { 'k', false, [&](const char *) { launcher.WeakContainer = false; } },
            { 'M', false, [&](const char *) { launcher.MergeLayers = true; } },
        });

        if (squash && compression.empty())
            compression = "squashfs";

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

        if (!bootstrap2_script.IsEmpty() && !bootstrap2_script.Exists()) {
            std::cerr << "Bootstrap2 " << bootstrap2_script << " not exists" << std::endl;
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

        if (!outputImage.IsEmpty() && !squash) {
            error = loopStorage.MkdirTmp(outputImage.DirName(), "loop.", 0755);
            if (error) {
                std::cerr << "Cannot create storage for loop: " << error << std::endl;
                return EXIT_FAILURE;
            }
            launcher.VolumeBackend = "loop";
            launcher.VolumeStorage = loopStorage.ToString();
            loopImage = loopStorage / "loop.img";
        }

        /* Start os in chroot container */
        chroot.StartOS = launcher.StartOS;
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
            bootstrap.Environment = launcher.Environment;

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
        }

        if (!bootstrap2_script.IsEmpty()) {
            bootstrap2.Container = launcher.Container + "/bootstrap2";
            bootstrap2.ForwardStreams = true;
            bootstrap2.WaitExit = true;
            bootstrap2.StartOS = true;
            bootstrap2.Environment = launcher.Environment;

            std::string script_text;
            error = bootstrap2_script.ReadAll(script_text);
            if (!error)
                error = volume_script.WriteAll(script_text);
            if (error) {
                std::cout << "Cannot copy script: " << error << std::endl;
                goto err;
            }

            bootstrap2.SetProperty("stdin_path", "/dev/null");
            bootstrap2.SetProperty("isolate", "true");
            bootstrap2.SetProperty("net", "inherited");
            bootstrap2.SetProperty("root", volume);
            bootstrap2.SetProperty("command", "/bin/bash -e -x -c '. ./script'");

            std::cout << "Starting bootstrap2 " << bootstrap2_script << std::endl;

            error = bootstrap2.Launch();
            if (error) {
                std::cout << "Cannot start bootstrap2: " << error << std::endl;
                goto err;
            }

            if (bootstrap2.ExitCode) {
                std::cout << "Bootstrap2: " << bootstrap2.ExitMessage << std::endl;
                goto err;
            }

            bootstrap2.Cleanup();
        }

        chroot.Container = launcher.Container + "/chroot";
        chroot.Environment = launcher.Environment;
        chroot.SetProperty("root", volume);
        chroot.SetProperty("isolate", "true");
        chroot.SetProperty("net", "inherited");

        if (chroot.StartOS)
            std::cout << "Starting OS" << std::endl;

        error = chroot.Launch();
        if (error) {
            std::cerr << "Cannot start chroot container: " << error << std::endl;
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

            executor.Container = chroot.Container + "/script";
            executor.ForwardStreams = true;
            executor.WaitExit = true;
            executor.Environment = launcher.Environment;

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

            if (Api->ExportLayer(volume, output.ToString(), compression)) {
                std::cerr << "Cannot export layer:" << launcher.GetLastError() << std::endl;
                goto err;
            }
        }

        if (squash) {
            std::cout << "Exporting squashfs into " << outputImage.ToString() << std::endl;

            if (Api->ExportLayer(volume, outputImage.ToString(), compression)) {
                std::cerr << "Cannot export layer:" << launcher.GetLastError() << std::endl;
                goto err;
            }
        }

        if (launcher.WeakContainer)
            launcher.Cleanup();

        if (!outputImage.IsEmpty() && !squash) {
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
                                           "[-s container] [-d container] <path>",
                                           "convert paths between different containers",
                                           "    -s container    source container (client container if omitted)\n"
                                           "    -d container    destination container (client container if omitted)\n") { }

    int Execute(TCommandEnviroment *environment) final override {
        std::string path, src, dest;
        auto args  = environment->GetOpts({
                {'s', true, [&](const char *arg) { src = arg; }},
                {'d', true, [&](const char *arg) { dest = arg; }}
        });

        if (args.size() != 1) {
            PrintError("Require exactly one agrument");
            return EXIT_FAILURE;
        }

        path = args[0];

        std::string converted;
        auto ret = Api->ConvertPath(path, src, dest, converted);
        if (ret)
            PrintError("Can't convert path");
        else
            std::cout << converted << std::endl;
        return ret;
    }
};

class TAttachCmd final : public ICmd {
public:
    TAttachCmd(Porto::Connection *api) : ICmd(api, "attach", 2,
            "<name> <pid> [comm]", "move process into container") { }

    int Execute(TCommandEnviroment *environment) final override {
        std::string comm;
        auto args = environment->GetArgs();
        auto name = args[0];
        int pid;

        if (StringToInt(args[1], pid)) {
            std::cerr << "Cannot parse pid " << args[0] << std::endl;
            return EXIT_FAILURE;
        }

        if (args.size() > 2)
            comm = args[2];
        else
            comm = GetTaskName(pid);

        auto ret = Api->AttachProcess(name, pid, comm);
        if (ret)
            PrintError("Cannot attach");
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
    handler.RegisterCommand<TRespawnCmd>();
    handler.RegisterCommand<TGetPropertyCmd>();
    handler.RegisterCommand<TSetPropertyCmd>();
    handler.RegisterCommand<TGetDataCmd>();
    handler.RegisterCommand<TGetCmd>();
    handler.RegisterCommand<TRawCmd>();
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
    handler.RegisterCommand<TStorageCmd>();

    handler.RegisterCommand<TConvertPathCmd>();
    handler.RegisterCommand<TAttachCmd>();

    int ret = handler.HandleCommand(argc, argv);
    if (ret < 0) {
        return EXIT_FAILURE;
    } else {
        return ret;
    }
}
