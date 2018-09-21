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

using TString;
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
    TString VirtMode = "app";
    bool ForwardTerminal = false;
    bool ForwardStreams = false;
    bool WaitExit = false;

    TString Container;
    std::vector<std::pair<TString, TString>> Properties;
    std::vector<TString> Environment;

    TString Private;

    TString VolumePath;
    TString SpaceLimit;
    TString VolumeBackend;
    TString VolumeStorage;
    TString Place;
    std::vector<TString> Layers;
    std::vector<TString> VolumeLayers;
    std::vector<TString> ImportedLayers;
    bool ContainerCreated = false;
    bool VolumeLinked = false;
    int LayerIndex = 0;

    int MasterPty = -1;
    int SlavePty = -1;

    int WaitTimeout = -1;

    int ExitCode = -1;
    int ExitSignal = -1;
    TString ExitMessage = "";

    ~TLauncher() {
        CloseSlavePty();
        CloseMasterPty();
    }

    TError GetLastError() {
        TString msg;
        auto error = Api->GetLastError(msg);
        return TError(error, msg);
    }

    TError SetProperty(const TString &key, const TString &val) {

        if (key == "virt_mode")
            VirtMode = val;

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

    TError SetProperty(const TString &prop) {
        TString::size_type n = prop.find('=');
        if (n == TString::npos)
            return TError(EError::InvalidValue, "Invalid value: " + prop);
        TString key = prop.substr(0, n);
        TString val = prop.substr(n + 1);
        return SetProperty(key, val);
    }

    TError ImportLayer(const TPath &path, TString &id) {
        id = "_weak_portoctl-" + std::to_string(GetPid()) + "-" +
             std::to_string(LayerIndex++) + "-" + path.BaseName();
        std::cout << "Importing layer " << path << " as " << id << std::endl;
        if (Api->ImportLayer(id, path.ToString(), false, Place))
            return GetLastError();
        ImportedLayers.push_back(id);
        return OK;
    }

    TError ImportLayers() {
        std::vector<TString> known;
        TError error;

        if (Api->ListLayers(known, Place))
            return GetLastError();

        for (auto &layer : Layers) {
            bool found = false;
            for (auto &l: known) {
                if (l == layer) {
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
                TString id;
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
        std::map<TString, TString> config;
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

        if (Api->CreateVolume(VolumePath, config))
            return GetLastError();
        VolumeLinked = true;

        if (Container != "") {
            if (Api->LinkVolume(VolumePath, Container))
                return GetLastError();
            if (Api->UnlinkVolume(VolumePath))
                return GetLastError();
            VolumeLinked = false;
        }

        return OK;
    }

    TError WaitContainer(int timeout) {
        TString result, oom_killed;
        TError error;
        int status;

        if (Api->WaitContainer(Container, result, timeout))
            return GetLastError();

        if (result == "timeout")
            return TError(EError::Busy, "Wait timeout");

        if (Api->GetProperty(Container, "exit_status", result) ||
                Api->GetProperty(Container, "oom_killed", oom_killed))
            return GetLastError();

        error = StringToInt(result, status);
        if (!error) {
            if (oom_killed == "true") {
                ExitSignal = 9;
                ExitCode = -99;
                ExitMessage = "Container killed by OOM";
            } else if (WIFSIGNALED(status)) {
                ExitSignal = WTERMSIG(status);
                ExitCode = -ExitSignal;
                ExitMessage = fmt::format("Container killed by signal: {} ({})",
                                            ExitSignal, strsignal(ExitSignal));
            } else if (WIFEXITED(status)) {
                ExitSignal = 0;
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
                    if (buf[i] == '\30') // ^X
                        escape++;
                    else
                        escape = 0;
                }

                if (escape >= 2) {
                    fmt::print(stderr, "\n\r<exit portoctl>\n\r");
                    WaitExit = false;
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
            TString tty = "/dev/fd/" + std::to_string(SlavePty);
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
            if (Api->SetProperty(Container, "root", ChrootVolume ? VolumePath : "/"))
                goto err;
            if (Api->SetProperty(Container, "cwd", ChrootVolume ? "/" : VolumePath))
                goto err;
        }

        for (auto &prop : Properties) {
            if (Api->SetProperty(Container, prop.first, prop.second))
                goto err;
        }

        if (Api->SetProperty(Container, "virt_mode", VirtMode))
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
            TString term = getenv("TERM") ?: "xterm";
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
            if (Api->UnlinkVolume(VolumePath))
                std::cerr << "Cannot unlink volume " << VolumePath << " : " << GetLastError() << std::endl;
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

static const TString StripIdx(const TString &name) {
    auto idx = name.find('[');
    if (idx != TString::npos)
        return TString(name.c_str(), idx);
    else
        return name;
}

static TString HumanValue(const TString &full_name, const TString &val, bool multiline = false) {
    auto name = StripIdx(full_name);
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
            name == "labels" ||
            name == "net" ||
            name == "ip" ||
            name == "default_gw" ||
            name == "devices" ||
            name == "bind" ||
            name == "symlink" ||
            name == "ulimit" ||
            name == "cgroups" ||
            name == "controllers" ||
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
            name == "io_ops" ||
            StringStartsWith(name, "capabilities")) {
                if (multiline)
                    return StringReplaceAll(val, ";", ";\n      ");
                return val;
    }

    if (name == "net_limit" ||
         name == "net_rx_limit" ||
         name == "net_guarantee" ||
         name == "net_bytes" ||
         name == "net_rx_bytes" ||
         name == "net_tx_bytes" ||
         name == "place_usage" ||
         name == "place_limit" ||
         name == "io_limit" ||
         name == "io_read" ||
         name == "io_write" ||
         name == "virtual_memory") {
        if (name != full_name && !StringToUint64(val, num)) {
            return StringFormatSize(num);
        } else if (!StringToUintMap(val, map)) {
            std::stringstream str;
            for (auto kv : map) {
                if (str.str().length())
                    str << ( multiline ? ";\n       " : "; " );
                str << kv.first << ": " << StringFormatSize(kv.second);
            }
            return str.str();
        } else if (multiline)
            return StringReplaceAll(val, ";", ";\n      ");
        return val;
    }

    if (name == "io_time") {
        if (name != full_name && !StringToUint64(val, num)) {
            return StringFormatDuration(num / 1000000);
        } else if (!StringToUintMap(val, map)) {
            std::stringstream str;
            for (auto kv : map) {
                if (str.str().length())
                    str << (multiline ? ";\n       " : "; ");
                str << kv.first << ": " << StringFormatDuration(kv.second / 1000000);
            }
            return str.str();
        } else if (multiline)
            return StringReplaceAll(val, ";", ";\n      ");
        return val;
    }

    if (val == "" || StringToUint64(val, num))
        return val;

    if (name == "memory_guarantee" ||
        name == "memory_limit" ||
        name == "memory_usage" ||
        name == "memory_limit_total" ||
        name == "memory_guarantee_total" ||
        name == "memory_reclaimed" ||
        name == "anon_usage" ||
        name == "anon_max_usage" ||
        name == "cache_usage" ||
        name == "anon_limit" ||
        name == "anon_limit_total" ||
        name == "max_rss" ||
        name == "stdout_limit" ||
        name == "hugetlb_limit" ||
        name == "hugetlb_usage")
        return StringFormatSize(num);

    if (name == "time" || name == "aging_time")
        return StringFormatDuration(num * 1000);

    if (name == "cpu_usage" ||
        name == "cpu_usage_system" ||
        name == "cpu_wait" ||
        name == "cpu_throttled" ||
        name == "cpu_period" ||
        name == "respawn_delay")
        return StringFormatDuration(num / 1000000);

    if (name == "exit_status")
        return FormatExitStatus(num);

    return val;
}

class TRawCmd final : public ICmd {
public:
    TRawCmd(Porto::Connection *api) : ICmd(api, "raw", 1, "<request>", "send request in text protobuf") {}

    int Execute(TCommandEnviroment *env) final override {
        stringstream req;

        const auto &args = env->GetArgs();
        copy(args.begin(), args.end(), ostream_iterator<TString>(req, " "));

        TString rsp;
        int ret = Api->Call(req.str(), rsp);
        std::cout << rsp << std::endl;

        return ret;
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
            { 's', false, [&](const char *) { flags |= Porto::GET_SYNC; } },
        });

        for (size_t i = 1; i < args.size(); ++i) {
            TString value;
            int ret = Api->GetProperty(args[0], args[i], value, flags);
            if (ret) {
                PrintError("Can't get property");
                return ret;
            }
            if (printKey)
                fmt::print("{} = {}\n", args[i], value);
            else
                fmt::print("{}\n", value);
        }

        return 0;
    }
};

class TSetPropertyCmd final : public ICmd {
public:
    TSetPropertyCmd(Porto::Connection *api) : ICmd(api, "set", 3, "<container> <property>", "set container property") {}

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        TString val = args[2];
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
            { 's', false, [&](const char *) { flags |= Porto::GET_SYNC; } },
        });

        for (size_t i = 1; i < args.size(); ++i) {
            TString value;
            int ret = Api->GetProperty(args[0], args[i], value, flags);
            if (ret) {
                PrintError("Can't get data");
                return ret;
            }
            if (printKey)
                fmt::print("{} = {}\n", args[i], value);
            else
                fmt::print("{}\n", value);
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

static const map<TString, int> sigMap = {
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
#ifdef SIGCLD
    { "SIGCLD",     SIGCLD },
#endif
    { "SIGPWR",     SIGPWR },
#ifdef SIGINFO
    { "SIGINFO",    SIGINFO },
#endif
#ifdef SIGLOST
    { "SIGLOST",    SIGLOST },
#endif
    { "SIGWINCH",   SIGWINCH },
    { "SIGSYS",     SIGSYS },
};

class TKillCmd final : public ICmd {
public:
    TKillCmd(Porto::Connection *api) : ICmd(api, "kill", 1, "<container> [signal]", "send signal to container") {}

    int Execute(TCommandEnviroment *env) final override {
        int sig = SIGTERM;
        const auto &args = env->GetArgs();
        if (args.size() >= 2) {
            const TString &sigName = args[1];

            if (sigMap.find(sigName) != sigMap.end()) {
                sig = sigMap.at(sigName);
            } else {
                TError error = StringToInt(sigName, sig);
                if (error) {
                    PrintError("Invalid signal", error);
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
        bool multiline = isatty(STDOUT_FILENO);
        bool printKey = true;
        bool printErrors = false;
        bool printEmpty = false;
        bool printHuman = true;
        bool multiGet = false;
        int flags = 0;
        std::vector<TString> list;
        std::vector<TString> vars;
        int ret;

        const auto &args = env->GetOpts({
                {'n', false, [&](const char *) { flags |= Porto::GET_NONBLOCK; }},
                {'s', false, [&](const char *) { flags |= Porto::GET_SYNC; }},
                {'r', false, [&](const char *) { flags |= Porto::GET_REAL; }},
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
            auto plist = Api->ListProperties();
            if (!plist) {
                PrintError("Can't list properties");
                return EXIT_FAILURE;
            }
            vars.reserve(plist->list_size());
            for (auto p : plist->list())
                vars.push_back(p.name());
            std::sort(vars.begin(), vars.end());
        }

        auto result = Api->Get(list, vars, flags);
        if (!result) {
            PrintError("Can't get containers' data");
            return ret;
        }

        if (result->list_size() > 1)
            multiGet = true;

        ret = EXIT_SUCCESS;

        for (auto &ct: result->list()) {

            if (multiGet)
                fmt::print("{}:\n", ct.name());

            for (const auto &kv : ct.keyval()) {
                auto &key = kv.variable();
                if (kv.error()) {
                    if (printErrors || key == "state") {
                        TError error(kv.error(), kv.errormsg());
                        PrintError("Cannot get " + key, error);
                        ret = EXIT_FAILURE;
                    }
                    continue;
                }

                auto val = kv.value();
                if (val.empty() && !printEmpty)
                continue;

                if (printHuman)
                    val = HumanValue(key, val, multiline);

                if (printKey)
                    fmt::print("{} = {}\n", key, val);
                else
                    fmt::print("{}\n", val);
            }

            if (multiGet)
                fmt::print("\n");
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
        "Execute command in container, forward terminal, destroy container at the end, exit - ^X^X",
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
            if (StringStartsWith(kv.first, "command_argv"))
                command_found = true;
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
            "start shell (default /bin/bash) in container, exit - ^X^X") { }

    int Execute(TCommandEnviroment *environment) final override {
        TString current_user = getenv("SUDO_USER") ?: getenv("USER") ?: "unknown";
        TString command = "/bin/bash";
        TString user, group;

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

        unsigned name_max = getuid() ? CONTAINER_PATH_MAX_FOR_SUPERUSER : CONTAINER_PATH_MAX;
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
        vector<TString> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        for (const auto &c : clist) {
            if (c == "/")
                continue;

            TString state;
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
        TString comm;
        if (error) {
            std::cerr << "Can't parse pid " << args[0] << std::endl;
            return EXIT_FAILURE;
        }

        if (args.size() > 1)
            comm = args[1];
        else
            comm = GetTaskName(pid);

        TString name;
        if (Api->LocateProcess(pid, comm, name)) {
            PrintError("Cannot find container by pid");
            return EXIT_FAILURE;
        }

        fmt::print("{}\n", name);

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
             "[-A] [-T <seconds>] <container|wildcard> ...",
             "Wait for any listed container change state to dead or meta without running children",
             "    -T <seconds>  timeout\n"
             "    -L <label>    wait for label\n"
             "    -A            async wait\n"
             ) {}

    static void PrintAsyncWait(const Porto::rpc::TContainerWaitResponse &event) {
        if (event.has_label())
            fmt::print("{} {}\t{}\t{} = {}\n", FormatTime(event.when()),
                       event.state(), event.name(), event.label(), event.value());
        else
            fmt::print("{} {}\t{}\n", FormatTime(event.when()),
                       event.state(), event.name());
        if (event.state() == "timeout")
            exit(0);
    }

    int Execute(TCommandEnviroment *env) final override {
        int timeout = -1;
        bool async = false;
        std::vector<TString> labels;
        const auto &containers = env->GetOpts({
            { 't', true, [&](const char *arg) { timeout = (std::stoi(arg) + 999) / 1000; } },
            { 'T', true, [&](const char *arg) { timeout = std::stoi(arg); } },
            { 'L', true, [&](const char *arg) { labels.push_back(arg); } },
            { 'A', false, [&](const char *) { async = true; } },
        });

        if (async) {
            int ret = Api->AsyncWait(containers.empty() ? std::vector<TString>({"***"}) : containers, labels, PrintAsyncWait, timeout);
            if (ret) {
                PrintError("Can't wait for containers");
                return ret;
            }
            Api->RecvAsyncWait();
            return 0;
        }

        if (containers.empty()) {
            PrintUsage();
            return EXIT_FAILURE;
        }

        TString name, state;
        auto rsp = Api->Wait(containers, labels, timeout);
        if (!rsp) {
            PrintError("Can't wait for containers");
            return EXIT_FAILURE;
        }

        if (rsp->name() == "")
            std::cerr << "timeout" << std::endl;
        else
            std::cout << rsp->name() << std::endl;

        return 0;
    }
};

class TListCmd final : public ICmd {
public:
    TListCmd(Porto::Connection *api) : ICmd(api, "list", 0,
            "[-1] [-f] [-r] [-t] [-L label[=value]] [pattern]",
            "list containers",
            "    -1        only names\n"
            "    -f        forest\n"
            "    -r        only running\n"
            "    -t        only toplevel\n"
            "    -L        only with label\n"
            "\n"
            "patterns:\n"
            " \"***\"        all containres\n"
            " \"*\"          all first level\n"
            " \"foo*/bar*\"  all matching\n"
            "\n"
            ) {}

    size_t CountChar(const TString &s, const char ch) {
        size_t count = 0;
        for (size_t i = 0; i < s.length(); i++)
            if (s[i] == ch)
                count++;
        return count;
    }

    TString GetParent(const TString &child) {
        auto lastSlash = child.rfind("/");
        if (lastSlash == TString::npos)
            return "/";
        else
            return child.substr(0, lastSlash);
    }

    int Execute(TCommandEnviroment *env) final override {
        bool details = true;
        bool forest = false;
        bool toplevel = false;
        bool running = false;
        TString label;
        const auto &args = env->GetOpts({
            { '1', false, [&](const char *) { details = false; } },
            { 'f', false, [&](const char *) { forest = true; } },
            { 't', false, [&](const char *) { toplevel = true; } },
            { 'L', true,  [&](const char *arg) { label = arg; } },
            { 'r', false, [&](const char *) { running = true; } },
        });
        TString mask = args.size() ? args[0] : "";
        int ret;

        vector<TString> clist;
        if (label != "" ) {
            auto sep = label.find('=');

            rpc::TPortoRequest req;
            rpc::TPortoResponse rsp;
            auto cmd = req.mutable_findlabel();

            if (sep == TString::npos) {
                cmd->set_label(label);
            } else {
                cmd->set_label(label.substr(0, sep));
                cmd->set_value(label.substr(sep + 1));
            }

            ret = Api->Call(req, rsp);
            if (!ret)
                for (auto &l: rsp.findlabel().list())
                    clist.push_back(l.name());
        } else
            ret = Api->List(clist, mask);

        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        if (clist.empty())
            return EXIT_SUCCESS;

        vector<TString> displayName;
        std::copy(clist.begin(), clist.end(), std::back_inserter(displayName));

        if (forest)
            for (size_t i = 0; i < clist.size(); i++) {
                auto c = clist[i];

                TString parent = GetParent(c);
                if (parent != "/") {
                    TString prefix = " ";
                    for (size_t j = 1; j < CountChar(displayName[i], '/'); j++)
                            prefix = prefix + "   ";

                    displayName[i] = prefix + "\\_ " + displayName[i].substr(parent.length() + 1);
                }
            }

        const std::vector<TString> vars = { "state", "time" };
        auto result = Api->Get(clist, vars);
        if (!result) {
            PrintError("Can't get containers' data");
            return ret;
        }

        vector<TString> states = { "running", "dead", "meta", "stopped", "paused" };
        size_t stateLen = MaxFieldLength(states);
        size_t nameLen = MaxFieldLength(displayName);
        size_t timeLen = 12;
        for (size_t i = 0; i < clist.size(); i++) {
            auto c = clist[i];
            auto &ct_state = result->list(i).keyval(0);
            auto &ct_time = result->list(i).keyval(1);
            if (c == "/")
                continue;

            if (ct_state.error() == EError::ContainerDoesNotExist)
                continue;

            if (toplevel && CountChar(c, '/'))
                continue;

            if (running && ct_state.value() != "running")
                continue;

            if (details)
                std::cout << std::left << std::setw(nameLen);

            std::cout << displayName[i];

            if (details) {
                if (ct_state.error() == EError::Busy)
                    std::cout << std::right << std::setw(stateLen) << "busy";
                else if (ct_state.error())
                    std::cout << std::right << std::setw(stateLen) << ct_state.errormsg();
                else
                    std::cout << std::right << std::setw(stateLen) << ct_state.value();

                bool showTime = ct_state.value() == "running" ||
                                ct_state.value() == "meta" ||
                                ct_state.value() == "dead";
                if (showTime && !ct_time.error())
                    std::cout << std::right << std::setw(timeLen)
                              << HumanValue("time", ct_time.value());
            }

            std::cout << std::endl;
        }

        return EXIT_SUCCESS;
    }
};

class TSortCmd final : public ICmd {
public:
    TSortCmd(Porto::Connection *api) : ICmd(api, "sort", 0,
            "[container|pattern... --] [property...]",
            "print containers sorted by resource usage",
            ""
            ) {}

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();

        std::vector<TString> filter;
        std::vector<TString> keys;

        auto sep = std::find(args.begin(), args.end(), "--");
        if (sep == args.end()) {
            filter.push_back("***");
            keys.insert(keys.end(), args.begin(), args.end());
        } else {
            filter.insert(filter.end(), args.begin(), sep);
            keys.insert(keys.end(), sep + 1, args.end());
        }

        if (!keys.size()) {
            keys.push_back("state");
            keys.push_back("time");
        }

        auto result = Api->Get(filter, keys, Porto::GET_SYNC);
        if (!result) {
            PrintError("Cannot get data");
            return EXIT_FAILURE;
        }

        std::vector<unsigned> index;
        index.resize(result->list_size());
        for (unsigned i = 0; i < result->list_size(); i++)
            index[i] = i;

        std::sort(index.begin(), index.end(),
                  [&](unsigned a, unsigned b) -> bool {
                      for (unsigned key = 0; key < keys.size(); key++) {
                          auto &a_str = result->list(a).keyval(key).value();
                          auto &b_str = result->list(b).keyval(key).value();
                          if (a_str == b_str)
                              continue;
                          int64_t a_int, b_int;
                          if (!StringToInt64(a_str, a_int) && !StringToInt64(b_str, b_int)) {
                              if (a_int == b_int)
                                  continue;
                              return a_int > b_int;
                          }
                          return a_str > b_str;
                      }
                      return a > b;
                  });

        TMultiTuple text;
        std::vector<unsigned> width;

        text.resize(index.size() + 1);
        text[0].resize(keys.size() + 1);
        width.resize(keys.size() + 1);

        text[0][0] = "container";
        width[0] = 16;
        for (unsigned col = 0; col < keys.size(); col++) {
            text[0][col + 1] = keys[col];
            width[col + 1] = std::max(8u, (unsigned)keys[col].size());
        }

        for (unsigned idx = 0; idx < index.size(); idx++) {
            auto &data = result->list(index[idx]);
            auto &name = data.name();
            text[idx + 1].resize(keys.size() + 1);
            text[idx + 1][0] = name;
            width[0] = std::max(width[0], (unsigned)name.size());
            for (unsigned col = 0; col < keys.size(); col++) {
                TString value = HumanValue(keys[col], data.keyval(col).value());
                text[idx + 1][col + 1] = value;
                width[col + 1] = std::max(width[col + 1], (unsigned)value.size());
            }
        }

        for (unsigned idx = 0; idx <= index.size(); idx++) {
            auto &line = text[idx];
            fmt::print("{: <{}} ", line[0], width[0]);
            for (unsigned col = 1; col < line.size(); col++)
                fmt::print("{: >{}} ", line[col], width[col]);
            fmt::print("\n");
        }

        return EXIT_SUCCESS;
    }
};

class TCreateVolumeCmd final : public ICmd {
public:
    TCreateVolumeCmd(Porto::Connection *api) : ICmd(api, "vcreate", 1, "-A|<path> [property=value...]",
        "create volume",
        "    -A        choose path automatically\n"
        ) {}

    int Execute(TCommandEnviroment *env) final override {
        std::map<TString, TString> properties;
        const auto &args = env->GetArgs();
        TString path = args[0];

        if (path == "-A") {
            path = "";
        } else {
            path = TPath(path).AbsolutePath().NormalPath().ToString();
        }

        for (size_t i = 1; i < args.size(); i++) {
            const TString &arg = args[i];
            std::size_t sep = arg.find('=');
            if (sep == TString::npos)
                properties[arg] = "";
            else
                properties[arg.substr(0, sep)] = arg.substr(sep + 1);
        }

        int ret = Api->CreateVolume(path, properties);
        if (ret) {
            PrintError("Can't create volume");
            return ret;
        }

        if (args[0] == "-A")
            fmt::print("{}\n", path);

        return 0;
    }
};

class TLinkVolumeCmd final : public ICmd {
public:
    TLinkVolumeCmd(Porto::Connection *api) : ICmd(api, "vlink", 1,
            "[-R] <path> [container] [target] [ro|rw]",
            "link volume to container", "default container - self\n") {}

    int Execute(TCommandEnviroment *env) final override {
        bool required = false;
        const auto &args = env->GetOpts({
                {'R', false, [&](const char *) { required = true; }},
        });
        const auto path = TPath(args[0]).AbsolutePath().NormalPath().ToString();
        int ret = Api->LinkVolume(path,
                (args.size() > 1) ? args[1] : "",
                (args.size() > 2) ? args[2] : "",
                (args.size() > 3) && args[3] == "ro",
                required);
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
        const auto path = TPath(args[0]).AbsolutePath().NormalPath().ToString();
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

    TString GetProp(const rpc::TVolumeDescription &v, const TString &n) {
        for (auto &p: v.properties()) {
            if (p.name() == n)
                return p.value();
        }
        return "-";
    }

    TString GetSize(const rpc::TVolumeDescription &v, const TString &n) {
        uint64_t val;
        for (auto &p: v.properties()) {
            if (p.name() == n) {
                if (StringToUint64(p.value(), val))
                    return "err";
                return StringFormatSize(val);
            }
        }
        return "-";
    }

    double GetPerc(const rpc::TVolumeDescription &v, const TString &n, const TString &d) {
        uint64_t val = 0;
        uint64_t div = 0;

        for (auto &p: v.properties()) {
            if (p.name() == n) {
                if (StringToUint64(p.value(), val))
                    return -1;
            }
            if (p.name() == d) {
                if (StringToUint64(p.value(), div))
                    return -1;
            }
        }

        if (val + div == 0)
            return -1;

        return 100. * val / (val + div);
    }

    void ShowVolume(const rpc::TVolumeDescription &v) {
        if (!details) {
            fmt::print("{}\n", v.path());
        } else {
            fmt::print("{:<40}", v.path());
            if (v.path().length() > 40)
                fmt::print("\n{:<40}", "");

            fmt::print(" {:>7} {:<7}", GetProp(v, V_ID), GetProp(v, V_BACKEND));

            if (inodes) {
                fmt::print(" {:>9} {:>9} {:>9} {:>3.0f}%",
                           GetProp(v, V_INODE_LIMIT),
                           GetProp(v, V_INODE_USED),
                           GetProp(v, V_INODE_AVAILABLE),
                           GetPerc(v, V_INODE_USED, V_INODE_AVAILABLE));
            } else {
                fmt::print(" {:>9} {:>9} {:>9} {:>3.0f}%",
                           GetSize(v, V_SPACE_LIMIT),
                           GetSize(v, V_SPACE_USED),
                           GetSize(v, V_SPACE_AVAILABLE),
                           GetPerc(v, V_SPACE_USED, V_SPACE_AVAILABLE));
            }

            for (auto link: v.links()) {
                fmt::print(" {}", link.container());
                if (link.has_target())
                    fmt::print("({}{}{})", link.target(),
                               link.read_only() ? " ro" : "",
                               link.required() ? " !" : "");
            }

            fmt::print("\n");

            if (v.path().length() > 40)
                fmt::print("\n");
        }

        if (!verbose)
            return;

        fmt::print("  {:<20}", "containers");
        for (auto link: v.links())
            fmt::print(" {}", link.container());
        fmt::print("\n");

        for (auto link: v.links())
            if (link.has_target())
                    fmt::print("  {:<20} {} {}{}{}\n",
                               link.container(), link.target(),
                               link.read_only() ? " ro" : "",
                               link.required() ? " !" : "");

        for (auto kv: v.properties())
            fmt::print("  {:<20} {}\n", kv.name(), kv.value());

        fmt::print("\n");
    }

    int Execute(TCommandEnviroment *env) final override {
        TString container;
        const auto &args = env->GetOpts({
            { '1', false, [&](const char *) { details = false; } },
            { 'i', false, [&](const char *) { inodes = true; } },
            { 'v', false, [&](const char *) { verbose = true; details = false; } },
            { 'c', true , [&](const char *arg) { container = arg; } },
        });

        if (details)
            fmt::print("{:<40} {:>7} {:<7} {:>9} {:>9} {:>9} {:>4} {}\n",
                       "Volume", "ID", "Backend", "Limit", "Used", "Avail",
                       "Use%", "Containers");

        if (args.empty()) {
            auto vlist = Api->ListVolumes("", container);
            if (!vlist) {
                PrintError("Can't list volumes");
                return EXIT_FAILURE;
            }

            for (auto &v : vlist->volumes())
                ShowVolume(v);
        } else {
            int errors = 0;

            for (const auto &arg : args) {
                const auto path = TPath(arg).AbsolutePath().NormalPath().ToString();

                auto vlist = Api->ListVolumes(path, "");
                if (!vlist) {
                    PrintError(arg);
                    errors++;
                    continue;
                }
                for (auto &v : vlist->volumes())
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
        std::map<TString, TString> properties;
        const auto &args = env->GetArgs();
        const auto path = TPath(args[0]).AbsolutePath().NormalPath().ToString();

        for (size_t i = 1; i < args.size(); i++) {
            const TString &arg = args[i];
            std::size_t sep = arg.find('=');
            if (sep == TString::npos)
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
        "[-P <place>] [-M] -L|-R|-F [meta/][storage]",
        "Manage internal persistent volume storage",
        "    -P <place>               optional path to place\n"
        "    -L                       list existing storages\n"
        "    -R <storage>             remove storage\n"
        "    -F [days]                remove all unused for [days]\n"
        "    -S <private>             set private value\n"
        "    -I <storage> <archive>   import storage from archive\n"
        "    -E <storage> <archive>   export storage to archive\n"
        "    -c <compression>         override compression\n"
        "    -M                       meta storage\n"
        "    -r                       resize\n"
        "    -Q <space_limit>\n"
        "    -q <inode_limit>\n"
        ) {}

    int ret = EXIT_SUCCESS;
    bool list = false;
    bool remove = false;
    bool flush = false;
    bool import = false;
    bool export_ = false;
    bool meta = false;
    bool resize = false;
    TString place;
    TString private_;
    TString compression;
    uint64_t space_limit = 0;
    uint64_t inode_limit = 0;

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetOpts({
            { 'P', true,  [&](const char *arg) { place = arg;   } },
            { 'S', true,  [&](const char *arg) { private_ = arg;   } },
            { 'R', false, [&](const char *) { remove = true; } },
            { 'L', false, [&](const char *) { list = true;   } },
            { 'F', false, [&](const char *) { flush = true;   } },
            { 'I', false, [&](const char *) { import = true;   } },
            { 'E', false, [&](const char *) { export_ = true;   } },
            { 'M', false, [&](const char *) { meta = true;   } },
            { 'r', false, [&](const char *) { resize = true;   } },
            { 'c', true, [&](const char * arg) { compression = arg; } },
            { 'q', true, [&](const char * arg) {  StringToSize(arg, inode_limit); } },
            { 'Q', true, [&](const char * arg) { StringToSize(arg, space_limit); } },
        });

        TString archive;
        if (args.size() >= 2)
            archive = TPath(args[1]).AbsolutePath().ToString();

        if (flush) {
            uint64_t age = 0;
            if (args.size() >= 1) {
                if (StringToUint64(args[0], age))
                    return EXIT_FAILURE;
                age *= 60*60*24;
            }

            auto rsp = Api->ListStorages(place);
            if (!rsp) {
                PrintError("Cannot list storage paths");
                return EXIT_FAILURE;
            }

            std::vector<TString> list;
            for (const auto &s: rsp->storages()) {
                if (s.last_usage() >= age)
                    list.push_back(s.name());
            }

            ret = EXIT_SUCCESS;
            for (auto &name: list) {
                fmt::print("remove {}\n", name);
                if (Api->RemoveStorage(name, place)) {
                    PrintError("Cannot remove storage");
                    if (Api->Error() != EError::Busy)
                        ret = EXIT_FAILURE;
                }
            }

        } else if (list) {
            auto rsp = Api->ListStorages(place);
            if (!rsp) {
                PrintError("Cannot list storages");
                return EXIT_FAILURE;
            }

            for (const auto &s: rsp->meta_storages()) {
                fmt::print("meta {}\n", s.name());
                if (s.has_private_value())
                    fmt::print("\tprivate\t{}\n", s.private_value());
                if (s.has_owner_user())
                    fmt::print("\towner\t{}:{}\n", s.owner_user(), s.owner_group());
                if (s.has_last_usage())
                    fmt::print("\tusage\t{} ago\n", StringFormatDuration(s.last_usage() * 1000));
                fmt::print("\tspace_limit\t{}\n", StringFormatSize(s.space_limit()));
                fmt::print("\tspace_used\t{}\n", StringFormatSize(s.space_used()));
                fmt::print("\tspace_available\t{}\n", StringFormatSize(s.space_available()));
                fmt::print("\tinode_limit\t{}\n", s.inode_limit());;
                fmt::print("\tinode_used\t{}\n", s.inode_used());
                fmt::print("\tinode_available\t{}\n", s.inode_available());
                fmt::print("\n");
            }

            for (const auto &s: rsp->storages()) {
                fmt::print("{}\n", s.name());
                if (s.has_private_value())
                    fmt::print("\tprivate\t{}\n", s.private_value());
                if (s.has_owner_user())
                    fmt::print("\towner\t{}:{}\n", s.owner_user(), s.owner_group());
                if (s.has_last_usage())
                    fmt::print("\tusage\t{} ago\n", StringFormatDuration(s.last_usage() * 1000));
                fmt::print("\n");
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

        } else if (meta) {
            if (args.size() < 1)
                return EXIT_FAILURE;
            rpc::TPortoRequest req;
            rpc::TPortoResponse rsp;
            rpc::TMetaStorage *s;

            if (remove)
                s = req.mutable_removemetastorage();
            else if (resize)
                s = req.mutable_resizemetastorage();
            else
                s = req.mutable_createmetastorage();

            s->set_name(args[0]);
            if (place != "")
                s->set_place(place);
            if (private_ != "")
                s->set_private_value(private_);
            if (inode_limit)
                s->set_inode_limit(inode_limit);
            if (space_limit)
                s->set_space_limit(space_limit);

            ret = Api->Call(req, rsp);
            if (ret)
                PrintError("");

        } else if (remove) {
            if (args.size() < 1)
                return EXIT_FAILURE;
            ret = Api->RemoveStorage(args[0], place);
            if (ret)
                PrintError("Cannot remove storage");

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
    TString place;
    TString private_value;
    TString compression;

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

        TString path;
        if (args.size() >= 2)
            path = TPath(args[1]).AbsolutePath().ToString();

        if (import) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], path, false, place, private_value);
            if (ret)
                PrintError("Cannot import layer");

        } else if (export_ || squash) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ExportLayer(args[0], path, compression);
            if (ret)
                PrintError("Cannot export layer");

        } else if (merge) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], path, true, place, private_value);
            if (ret)
                PrintError("Cannot merge layer");

        } else if (remove) {
            if (args.size() < 1)
                return EXIT_FAILURE;

            for (const auto &arg : args) {
                if (Api->RemoveLayer(arg, place)) {
                    PrintError("Cannot remove layer");
                    ret = Api->Error();
                }
            }

        } else if (flush) {
            uint64_t age = 0;
            if (args.size() >= 1) {
                if (StringToUint64(args[0], age))
                    return EXIT_FAILURE;
                age *= 60*60*24;
            }

            std::vector<TString> list;
            auto rsp = Api->ListLayers(place);
            if (!rsp) {
                PrintError("Cannot list layers");
                return EXIT_FAILURE;
            }

            for (const auto &l: rsp->layers()) {
                if (l.last_usage() >= age)
                    list.push_back(l.name());
            }

            for (auto &name: list) {
                fmt::print("remove {}\n", name);
                if (Api->RemoveLayer(name, place)) {
                    PrintError("Cannot remove layer");
                    if (Api->Error() != EError::Busy)
                        ret = Api->Error();
                }
            }

        } else if (list) {
            auto rsp = Api->ListLayers(place);
            if (!rsp) {
                PrintError("Cannot list layers");
                return EXIT_FAILURE;
            }

            for (const auto &l: rsp->layers()) {
                fmt::print("{}\n", l.name());
                if (!verbose)
                    continue;
                if (l.private_value().size())
                    fmt::print("\tprivate\t{}\n", l.private_value());
                if (l.owner_user().size())
                    fmt::print("\towner\t{}:{}\n", l.owner_user(), l.owner_group());
                if (l.last_usage())
                    fmt::print("\tused\t{} ago\n", StringFormatDuration(l.last_usage() * 1000));;
                fmt::print("\n");
            }

        } else if (get_private) {
            if (args.size() < 1)
                return EXIT_FAILURE;
            ret = Api->GetLayerPrivate(private_value, args[0], place);
            if (ret)
                PrintError("Cannot get layer private value");
            else
                fmt::print("{}\n", private_value);

        } else if (set_private) {
            if (args.size() < 1)
                return EXIT_FAILURE;
            ret = Api->SetLayerPrivate(private_value, args[0], place);
            if (ret)
                PrintError("Cannot set layer private value");

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
            "[-k] [-M] [-l|-L layer]... [-o layer.tar] [-O image.img] [-Q image.squashfs] [-B|-b script] [-S|-s script]... [properties]...",
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
            "    -s script2                 bash script runs inside after os stop (with root=volume)\n"
            "    -M                         merge all layers together\n"
            "    -k                         keep volume and container\n"
            ) { }

    ~TBuildCmd() { }

    int Execute(TCommandEnviroment *environment) final override {
        TLauncher launcher(Api);
        TLauncher bootstrap(Api);
        TLauncher bootstrap2(Api);
        TLauncher chroot(Api);
        TLauncher base(Api);
        TError error;

        TString build_script = "porto_build";
        TString build_command = "/bin/bash -ex porto_build";

        launcher.Container = "portoctl-build-" + std::to_string(GetPid());
        launcher.SetProperty("isolate", "false");
        launcher.SetProperty("net", "NAT");
        launcher.WeakContainer = true;
        launcher.NeedVolume = true;
        launcher.ChrootVolume = false;
        launcher.VirtMode = "os";

        TPath output;
        TPath outputImage;
        bool squash = false;
        TString compression;
        TPath loopStorage, loopImage;
        std::vector<TString> env;
        std::vector<TString> layers;
        std::vector<TPath> scripts;
        std::vector<TPath> scripts2;
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
            { 's', true, [&](const char *arg) { scripts2.push_back(TPath(arg).RealPath()); } },
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

        for (auto &script: scripts2) {
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

        /* Start os in launcher/chroot/base container */
        base.VirtMode = launcher.VirtMode;
        launcher.VirtMode = "app";

        TString volume;
        TPath volume_script;

        error = launcher.Launch();
        if (error) {
            std::cerr << "Cannot create volume: " << error << std::endl;
            goto err;
        }

        volume = launcher.VolumePath;

        volume_script = volume + "/" + build_script;
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
            bootstrap.VirtMode = "os";
            bootstrap.Environment = launcher.Environment;

            TString script_text;
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
                /* allow devices at root for unpatched debootsrap */
                bootstrap.SetProperty("bind", volume + " " + volume + " rw,dev;/tmp /tmp rw");
                bootstrap.SetProperty("root_readonly" "true");
            } else {
                bootstrap.SetProperty("bind", volume + " " + volume + " rw");
            }

            bootstrap.SetProperty("cwd", volume);
            bootstrap.SetProperty("command", build_command);

            std::cout << "\nStarting bootstrap " << bootstrap_script << " ...\n" << std::endl;

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
            bootstrap2.VirtMode = "os";
            bootstrap2.Environment = launcher.Environment;

            TString script_text;
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
            bootstrap2.SetProperty("command", build_command);

            std::cout << "\nStarting bootstrap2 " << bootstrap2_script << " ...\n" << std::endl;

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

        error = chroot.Launch();
        if (error) {
            std::cerr << "Cannot start container: " << error << std::endl;
            goto err;
        }

        base.Container = launcher.Container + "/chroot/base";
        base.Environment = launcher.Environment;
        base.SetProperty("isolate", "true");
        base.SetProperty("net", "inherited");

        if (base.VirtMode == "os")
            std::cout << "\nStarting OS ..." << std::endl;

        error = base.Launch();
        if (error) {
            std::cerr << "Cannot start container: " << error << std::endl;
            goto err;
        }

        for (auto &script: scripts) {
            TLauncher executor(Api);

            TString script_text;
            error = script.ReadAll(script_text);
            if (!error)
                error = volume_script.WriteAll(script_text);
            if (error) {
                std::cout << "Cannot copy script: " << error << std::endl;
                goto err;
            }

            executor.Container = base.Container + "/script";
            executor.ForwardStreams = true;
            executor.WaitExit = true;
            executor.Environment = launcher.Environment;

            executor.SetProperty("stdin_path", "/dev/null");
            executor.SetProperty("isolate", "false");
            executor.SetProperty("virt_mode", "os");
            executor.SetProperty("net", "inherited");
            executor.SetProperty("command", build_command);

            std::cout << "\nStarting script " << script << " ...\n" << std::endl;

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

        if (base.VirtMode == "os")
            std::cout << "Stopping OS ..." << std::endl;

        error = base.StopContainer();
        if (error) {
            std::cerr << "Cannot stop container: " << error << std::endl;
            goto err;
        }

        for (auto &script: scripts2) {
            TLauncher executor(Api);

            TString script_text;
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
            executor.SetProperty("command", build_command);

            std::cout << "\nStarting script " << script << " ...\n" << std::endl;

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

        if (scripts.size() || scripts2.size()) {
            TLauncher executor(Api);

            volume_script.WriteAll(
                    "find root/run -xdev -mindepth 1 -delete\n"
                    "find run -maxdepth 1 -type d | "
                    "tar --create --no-recursion --files-from - | "
                    "tar --verbose --extract -C root\n");
            executor.Container = chroot.Container + "/save_run";
            executor.ForwardStreams = true;
            executor.WaitExit = true;

            executor.SetProperty("stdin_path", "/dev/null");
            executor.SetProperty("isolate", "false");
            executor.SetProperty("virt_mode", "os");
            executor.SetProperty("net", "inherited");
            executor.SetProperty("bind", "/ root");
            executor.SetProperty("command", build_command);

            std::cout << "\nSave directories in /run\n" << std::endl;

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

        error = chroot.StopContainer();
        if (error) {
            std::cerr << "Cannot stop chroot container: " << error << std::endl;
            goto err;
        }

        if (Api->SetProperty(chroot.Container, "command", "rm " + build_script) ||
                Api->SetProperty(chroot.Container, "virt_mode", "os") ||
                Api->Start(chroot.Container) || chroot.WaitContainer(-1)) {
            std::cerr << "Cannot remove script" << std::endl;
            goto err;
        }

        if (!scripts.empty() && launcher.StopContainer())
            goto err;

        if (!output.IsEmpty()) {
            std::cout << "\nExporting layer into " << output.ToString() << " ..." << std::endl;

            if (Api->ExportLayer(volume, output.ToString(), compression)) {
                std::cerr << "Cannot export layer:" << launcher.GetLastError() << std::endl;
                goto err;
            }
        }

        if (squash) {
            std::cout << "\nExporting squashfs into " << outputImage.ToString() << " ..." << std::endl;

            if (Api->ExportLayer(volume, outputImage.ToString(), compression)) {
                std::cerr << "Cannot export layer:" << launcher.GetLastError() << std::endl;
                goto err;
            }
        }

        if (launcher.WeakContainer)
            launcher.Cleanup();

        if (!outputImage.IsEmpty() && !squash) {
            std::cout << "\nExporting image into " << outputImage.ToString() << " ..." << std::endl;

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
        TString path, src, dest;
        auto args  = environment->GetOpts({
                {'s', true, [&](const char *arg) { src = arg; }},
                {'d', true, [&](const char *arg) { dest = arg; }}
        });

        if (args.size() != 1) {
            PrintError("Require exactly one agrument");
            return EXIT_FAILURE;
        }

        path = args[0];

        TString converted;
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
            "[-t] <container> <pid> [comm]", "move process or thread into container") { }

    int Execute(TCommandEnviroment *environment) final override {
        TString comm;
        bool thread = false;
        const auto &args = environment->GetOpts({
                {'t', false, [&](const char *) { thread = true; }},
        });
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

        int ret;
        if (thread)
            ret = Api->AttachThread(name, pid, comm);
        else
            ret = Api->AttachProcess(name, pid, comm);
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
