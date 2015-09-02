#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <csignal>
#include <cmath>

#include "libporto.hpp"
#include "config.hpp"
#include "cli.hpp"
#include "volume.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
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

static bool ValidData(const vector<TData> &dlist, const string &name) {
    return find_if(dlist.begin(), dlist.end(),
                   [&](const TData &i)->bool { return i.Name == StripIdx(name); })
        != dlist.end();
}

static bool ValidProperty(const vector<TProperty> &plist, const string &name) {
    return find_if(plist.begin(), plist.end(),
                   [&](const TProperty &i)->bool { return i.Name == StripIdx(name); })
        != plist.end();
}

static string HumanValue(const string &name, const string &val) {
    if (val == "")
        return val;

    if (name == "memory_guarantee" || name == "memory_limit") {
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
    } else if (name == "cpu_usage") {
        return HumanNsec(val);
    } else if (name == "time") {
        return HumanSec(val);
    }

    return val;
}

class TVolumeBuilder {
    TPortoAPI *Api;
    std::string Volume;
    std::vector<std::string> Layers;
    bool Cleanup = true;
    int LayerIdx = 0;

    TError GetLastError() {
        int error;
        std::string msg;
        Api->GetLastError(error, msg);
        return TError((EError)error, msg);
    }

    TError ImportLayer(const std::string &path, std::string &id) {
        id = "portoctl-" + std::to_string(GetPid()) + "-" + std::to_string(LayerIdx++);
        std::cout << "Importing layer " << path << "..." << std::endl;
        int ret = Api->ImportLayer(id, path);
        if (ret)
            return GetLastError();
        return TError::Success();
    }

    TError CreateOverlayVolume(const std::vector<std::string> &layers,
                               std::string &path) {
        TVolumeDescription volume;
        std::map<std::string, std::string> config;
        config["backend"] = "overlay";
        config["layers"] = CommaSeparatedList(layers, ";");
        int ret = Api->CreateVolume("", config, volume);
        if (ret)
            return GetLastError();

        path = volume.Path;
        return TError::Success();
    }

    TError CreatePlainVolume(std::string &path) {
        TVolumeDescription volume;
        std::map<std::string, std::string> config;
        int ret = Api->CreateVolume("", config, volume);
        if (ret)
            return GetLastError();

        path = volume.Path;
        return TError::Success();
    }

public:
    TVolumeBuilder(TPortoAPI *api) : Api(api) {}

    std::string GetVolumePath() {
        return Volume;
    }

    void NeedCleanup(bool cleanup) { Cleanup = cleanup; }

    ~TVolumeBuilder() {
        if (Cleanup) {
            if (!Volume.empty()) {
                int ret = Api->UnlinkVolume(Volume, "");
                if (ret)
                    std::cerr << "Can't unlink volume: " << GetLastError() << std::endl;
            }

            for (auto id : Layers) {
                int ret = Api->RemoveLayer(id);
                if (ret)
                    std::cerr << "Can't remove layer " << id << ": " << GetLastError() << std::endl;
            }
        }
    }

    TError Prepare() {
        return CreatePlainVolume(Volume);
    }

    TError Prepare(const std::vector<std::string> &layers) {
        for (auto layer : layers) {
            layer = StringTrim(layer);

            if (layer.empty())
                continue;

            std::string id;
            TPath path(layer);
            TError error = ImportLayer(path.RealPath().ToString(), id);
            if (error)
                return error;

            Layers.push_back(id);
        }

        return CreateOverlayVolume(Layers, Volume);
    }

    TError Prepare(const std::string &s) {
        std::vector<std::string> layers;

        TError error = SplitString(s, ';', layers);
        if (error)
            return error;

        return Prepare(layers);
    }

    TError ExportLayer(const TPath &path) {
        int ret = Api->ExportLayer(Volume, path.RealPath().ToString());
        if (ret)
            return GetLastError();
        return TError::Success();
    }

    TError Link(const std::string &container) {
        int ret = Api->LinkVolume(Volume, container);
        if (ret)
            return GetLastError();
        return TError::Success();
    }

    TError Unlink() {
        int ret = Api->UnlinkVolume(Volume, "");
        if (ret)
            return GetLastError();
        return TError::Success();
    }
};

class TRawCmd final : public ICmd {
public:
    TRawCmd(TPortoAPI *api) : ICmd(api, "raw", 1, "<message>", "send raw protobuf message") {}

    int Execute(int argc, char *argv[]) override {
        stringstream msg;

        std::vector<std::string> args(argv, argv + argc);
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

        string resp;
        if (!Api->Raw(msg.str(), resp))
            std::cout << resp << std::endl;

        return 0;
    }
};

class TCreateCmd final : public ICmd {
public:
    TCreateCmd(TPortoAPI *api) : ICmd(api, "create", 1, "<container1> [container2...]", "create container") {}

    int Execute(int argc, char *argv[]) override {
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

class TGetPropertyCmd final : public ICmd {
public:
    TGetPropertyCmd(TPortoAPI *api) : ICmd(api, "pget", 2, "[-k] <container> <property> [property...]", "get raw container property") {}

    int Execute(int argc, char *argv[]) override {
        bool printKey = false;
        int start = GetOpt(argc, argv, {
            { 'k', false, [&](const char *arg) { printKey = true; } },
        });

        for (int i = start + 1; i < argc; i++) {
            string value;
            int ret = Api->GetProperty(argv[start], argv[i], value);
            if (ret) {
                PrintError("Can't get property");
                return ret;
            }
            if (printKey)
                PrintPair(argv[i], value);
            else
                Print(value);
        }

        return 0;
    }
};

class TSetPropertyCmd final : public ICmd {
public:
    TSetPropertyCmd(TPortoAPI *api) : ICmd(api, "set", 3, "<container> <property>", "set container property") {}

    int Execute(int argc, char *argv[]) override {
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

class TGetDataCmd final : public ICmd {
public:
    TGetDataCmd(TPortoAPI *api) : ICmd(api, "dget", 2, "[-k] <container> <data> [data...]", "get raw container data") {}

    int Execute(int argc, char *argv[]) override {
        bool printKey = false;
        int start = GetOpt(argc, argv, {
            { 'k', false, [&](const char *arg) { printKey = true; } },
        });

        for (int i = start + 1; i < argc; i++) {
            string value;
            int ret = Api->GetData(argv[start], argv[i], value);
            if (ret) {
                PrintError("Can't get data");
                return ret;
            }
            if (printKey)
                PrintPair(argv[i], value);
            else
                Print(value);
        }

        return 0;
    }
};

class TStartCmd final : public ICmd {
public:
    TStartCmd(TPortoAPI *api) : ICmd(api, "start", 1, "<container1> [container2...]", "start container") {}

    int Execute(int argc, char *argv[]) override {
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

class TKillCmd final : public ICmd {
public:
    TKillCmd(TPortoAPI *api) : ICmd(api, "kill", 1, "<container> [signal]", "send signal to container") {}

    int Execute(int argc, char *argv[]) override {
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

class TStopCmd final : public ICmd {
public:
    TStopCmd(TPortoAPI *api) : ICmd(api, "stop", 1, "<container1> [container2...]", "stop container") {}

    int Execute(int argc, char *argv[]) override {
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

class TRestartCmd final : public ICmd {
public:
    TRestartCmd(TPortoAPI *api) : ICmd(api, "restart", 1, "<container1> [container2...]", "restart container") {}

    int Execute(int argc, char *argv[]) override {
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

class TPauseCmd final : public ICmd {
public:
    TPauseCmd(TPortoAPI *api) : ICmd(api, "pause", 1, "<container> [name...]", "pause container") {}

    int Execute(int argc, char *argv[]) override {
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

class TResumeCmd final : public ICmd {
public:
    TResumeCmd(TPortoAPI *api) : ICmd(api, "resume", 1, "<container1> [container2...]", "resume container") {}

    int Execute(int argc, char *argv[]) override {
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

class TGetCmd final : public ICmd {
public:
    TGetCmd(TPortoAPI *api) : ICmd(api, "get", 1, "<container> <variable> [variable...]", "get container property or data") {}

    int Execute(int argc, char *argv[]) override {
        string value;
        int ret;
        bool printKey = true;
        bool printErrors = true;

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

        std::string container = argv[0];
        std::vector<std::string> clist = { container };
        std::vector<std::string> vars;

        if (argc > 1) {
            for (int i = 1; i < argc; i++)
                vars.push_back(argv[i]);
            // we want to preserve old behavior:
            // - get without arguments prints all properties/data prefixed with name
            // - get with arguments prints specified properties/data without prefix
            printKey = false;
        } else {
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

        std::map<std::string, std::map<std::string, TPortoGetResponse>> result;
        ret = Api->Get(clist, vars, result);
        if (ret) {
            PrintError("Can't get containers' data");
            return ret;
        }

        auto &data = result[container];

        if (printErrors)
            for (auto key : vars) {
                if (data[key].Error) {
                    TError error((rpc::EError)data[key].Error, data[key].ErrorMsg);
                    PrintError(error, "Can't get " + key);
                    return EXIT_FAILURE;
                }
            }

        for (auto key : vars) {
            if (data[key].Error)
                continue;

            auto val = HumanValue(key, data[key].Value);
            if (printKey)
                PrintPair(key, val);
            else
                Print(val);
        }

        return EXIT_SUCCESS;
    }
};

class TEnterCmd final : public ICmd {
public:
    TEnterCmd(TPortoAPI *api) : ICmd(api, "enter", 1, "[-C] <container> [command]", "execute command in container namespace") {}

    void PrintErrno(const string &str) {
        std::cerr << str << ": " << strerror(errno) << std::endl;
    }

    TError GetCgMount(const string &subsys, TPath &root) {
        vector<string> subsystems;
        TError error = SplitString(subsys, ',', subsystems);
        if (error)
            return error;

        vector<shared_ptr<TMount>> mounts;
        error = TMount::Snapshot(mounts);
        if (error)
            return error;

        for (auto &mount : mounts) {
            bool found = true;
            for (auto &ss : subsystems)
                if (!mount->HasFlag(ss)) {
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

    int Execute(int argc, char *argv[]) override {
        bool enterCgroups = true;
        int start = GetOpt(argc, argv, {
            { 'C', false, [&](const char *arg) { enterCgroups = false; } },
        });

        string cmd = "";
        for (int i = start + 1; i < argc; i++) {
            cmd += argv[i];
            cmd += " ";
        }

        if (!cmd.length())
            cmd = "/bin/bash";

        string pidStr;
        int ret = Api->GetData(argv[start], "root_pid", pidStr);
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
                    PrintError(error, "Can't get task cgroups");
                    return EXIT_FAILURE;
                }

                TFile f(root / cg.second / "cgroup.procs");
                error = f.AppendString(std::to_string(GetPid()));
                if (error) {
                    PrintError(error, "Can't get task cgroups");
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
    TVolumeBuilder VolumeBuilder;
public:
    TRunCmd(TPortoAPI *api) : ICmd(api, "run", 2, "[-L layers] <container> [properties]", "create and start container with given properties"), VolumeBuilder(api) {}

    int Parser(string property, string &key, string &val) {
        string::size_type n;
        n = property.find('=');
        if (n == string::npos) {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property (no value): " + property);
            return EXIT_FAILURE;
        }
        key = property.substr(0, n);
        val = property.substr(n + 1, property.size());
        if (key == "" || val == "") {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property (key or value is nil): " + property);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    int Execute(int argc, char *argv[]) override {
        std::string layers;
        int start = GetOpt(argc, argv, {
            { 'L', true, [&](const char *arg) { layers = arg; } },
        });

        string containerName = argv[start];
        std::vector<std::pair<std::string, std::string>> properties;

        int ret;

        for (int i = start + 1; i < argc; i++) {
            string key, val;
            ret = Parser(argv[i], key, val);
            if (ret)
                return ret;

            properties.push_back(std::make_pair(key, val));
        }

        if (!layers.empty()) {
            TError error = VolumeBuilder.Prepare(layers);
            if (error) {
                std::cerr << "Can't create volume: " << error << std::endl;
                return EXIT_FAILURE;
            }

            properties.push_back(std::make_pair("root", VolumeBuilder.GetVolumePath()));
        }

        ret = Api->Create(containerName);
        if (ret) {
            PrintError("Can't create container");
            return EXIT_FAILURE;
        }
        if (!VolumeBuilder.GetVolumePath().empty()) {
            VolumeBuilder.Link(containerName);
            VolumeBuilder.Unlink();
            VolumeBuilder.NeedCleanup(false);
        }
        for (auto iter: properties) {
            ret = Api->SetProperty(containerName, iter.first, iter.second);
            if (ret) {
                PrintError("Can't set property " + iter.first);
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

class TExecCmd final : public ICmd {
    string containerName;
    struct termios SavedAttrs;
    bool Canonical = true;
    TVolumeBuilder VolumeBuilder;
    bool Cleanup = true;
    char *TmpDir = nullptr;
public:
    TExecCmd(TPortoAPI *api) : ICmd(api, "exec", 2, "[-C] [-T] [-L layers] <container> command=<command> [properties]", "create pty, execute and wait for command in container"), VolumeBuilder(api) {
        SetDieOnSignal(false);
    }

    ~TExecCmd() {
        if (Cleanup && !containerName.empty()) {
            TPortoAPI api(config().rpc_sock().file().path());
            (void)api.Destroy(containerName);
        }
        if (TmpDir) {
            TFolder dir(TmpDir);
            (void)dir.Remove(true);
        }
        if (!Canonical)
            tcsetattr(STDIN_FILENO, TCSANOW, &SavedAttrs);
    }

    int SwithToNonCanonical(int fd) {
        if (!isatty(fd))
            return 0;

        if (tcgetattr(fd, &SavedAttrs) < 0)
            return -1;
        Canonical = false;

        static struct termios t = SavedAttrs;
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

    int Execute(int argc, char *argv[]) override {
        bool hasTty = isatty(STDIN_FILENO);
        std::vector<std::string> args;
        std::string env;
        std::string layers;

        int start = GetOpt(argc, argv, {
            { 'C', false, [&](const char *arg) { Cleanup = false; } },
            { 'T', false, [&](const char *arg) { hasTty = false; } },
            { 'L', true, [&](const char *arg) { layers = arg; } },
        });
        VolumeBuilder.NeedCleanup(Cleanup);

        if (hasTty)
            env = std::string("TERM=") + getenv("TERM");

        args.push_back(argv[start]);
        for (int i = start + 1; i < argc; i++)
            if (strncmp(argv[i], "env=", 4) == 0)
                env = env + "; " + std::string(argv[i] + 4);
            else
                args.push_back(argv[i]);

        if (!layers.empty()) {
            TError error = VolumeBuilder.Prepare(layers);
            if (error) {
                std::cerr << "Can't create volume: " << error << std::endl;
                return EXIT_FAILURE;
            }
            args.push_back("root=" + VolumeBuilder.GetVolumePath());
        }

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
            TmpDir = mkdtemp(strdup("/tmp/portoctl-exec-XXXXXX"));
            if (!TmpDir) {
                TError error(EError::Unknown, errno, "mkdtemp()");
                PrintError(error, "Can't create temporary directory");
                return EXIT_FAILURE;
            }

            stdinPath = string(TmpDir) + "/stdin";
            if (MakeFifo(stdinPath))
                return EXIT_FAILURE;

            stdoutPath = string(TmpDir) + "/stdout";
            if (MakeFifo(stdoutPath))
                return EXIT_FAILURE;

            stderrPath = string(TmpDir) + "/stderr";
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

        args.push_back("stdin_path=" + stdinPath);
        args.push_back("stdout_path=" + stdoutPath);
        args.push_back("stderr_path=" + stderrPath);
        if (env.length())
            args.push_back("env=" + env);

        containerName = argv[start];
        int ret = RunCmd<TRunCmd>(args);
        if (ret)
            return ret;

        bool hangup = false;
        while (!hangup) {
            if (GotSignal())
                return EXIT_FAILURE;

            ret = poll(fds.data(), fds.size(), -1);
            if (ret < 0)
                break;

            for (size_t i = 0; i < fds.size(); i++) {
                if (fds[i].revents & POLLIN) {
                    if (fds[i].fd == STDIN_FILENO)
                        MoveData(STDIN_FILENO, stdinFd);
                    else if (fds[i].fd == stdoutFd)
                        MoveData(stdoutFd, STDOUT_FILENO);
                    else if (fds[i].fd == stderrFd)
                        MoveData(stderrFd, STDERR_FILENO);
                }

                if (fds[i].revents & POLLHUP) {
                    if (fds[i].fd != STDIN_FILENO)
                        hangup = true;
                    fds.erase(fds.begin() + i);
                }
            }
        }

        if (GotSignal())
            return EXIT_FAILURE;

        std::string tmp;
        ret = Api->Wait({ containerName }, tmp);
        if (GotSignal())
            return EXIT_FAILURE;
        if (ret) {
            PrintError("Can't get state");
            return EXIT_FAILURE;
        }

        string s;
        ret = Api->GetData(containerName, "exit_status", s);
        if (ret) {
            PrintError("Can't get exit_status");
            return EXIT_FAILURE;
        }

        int status = stoi(s);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else {
            Interrupted = 1;
            InterruptedSignal = WTERMSIG(status);
        }

        return EXIT_FAILURE;
    }
};

class TGcCmd final : public ICmd {
public:
    TGcCmd(TPortoAPI *api) : ICmd(api, "gc", 0, "", "remove all dead containers") {}

    int Execute(int argc, char *argv[]) override {
        vector<string> clist;
        int ret = Api->List(clist);
        if (ret) {
            PrintError("Can't list containers");
            return ret;
        }

        for (auto c : clist) {
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
    TFindCmd(TPortoAPI *api) : ICmd(api, "find", 1, "", "find container for given process id") {}

    int Execute(int argc, char *argv[]) override {
        int pid;
        TError error = StringToInt(argv[0], pid);
        if (error) {
            std::cerr << "Can't parse pid " << argv[0] << std::endl;
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
        auto prefix = "/" + PORTO_ROOT_CGROUP + "/";
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
    TDestroyCmd(TPortoAPI *api) : ICmd(api, "destroy", 1, "<container1> [container2...]", "destroy container") {}

    int Execute(int argc, char *argv[]) override {
        int exitStatus = EXIT_SUCCESS;
        for (int i = 0; i < argc; i++) {
            int ret = Api->Destroy(argv[i]);
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
    TWaitCmd(TPortoAPI *api) : ICmd(api, "wait", 1, "<container1> [container2] ...", "wait for listed containers") {}

    int Execute(int argc, char *argv[]) override {
        int timeout = -1;
        int start = GetOpt(argc, argv, {
            { 't', true, [&](const char *arg) { timeout = std::stoi(arg); } },
        });

        std::vector<std::string> containers;
        for (int i = start; i < argc; i++)
            containers.push_back(argv[i]);

        std::string name;
        int ret = Api->Wait(containers, name, timeout);
        if (ret) {
            PrintError("Can't wait for containers");
            return ret;
        }

        if (name.empty())
            std::cerr << "timeout" << std::endl;
        else
            std::cout << name << " isn't running" << std::endl;

        return 0;
    }
};

class TListCmd final : public ICmd {
public:
    TListCmd(TPortoAPI *api) : ICmd(api, "list", 0, "[-1] [-f] [-t]", "list created containers") {}

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

    int Execute(int argc, char *argv[]) override {
        bool details = true;
        bool forest = false;
        bool toplevel = false;
        (void)GetOpt(argc, argv, {
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
        std::map<std::string, std::map<std::string, TPortoGetResponse>> result;
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
            if (state.Error)
                continue;

            if (toplevel && CountChar(c, '/'))
                continue;

            if (details)
                std::cout << std::left << std::setw(nameLen);

            std::cout << displayName[i];

            if (details) {
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

extern int portotop(TPortoAPI *api, std::string config);
class TTopCmd final : public ICmd {
public:
    TTopCmd(TPortoAPI *api) : ICmd(api, "top", 0, "[config]", "top-like tool for container monitoring and control") {}

    int Execute(int argc, char *argv[]) override {
        if (argc == 0)
            return portotop(Api, "");
        else
            return portotop(Api, argv[0]);
    }
};

class TSortCmd final : public ICmd {
public:
    TSortCmd(TPortoAPI *api) : ICmd(api, "sort", 0, "[sort-by]", "print containers sorted by resource usage") {}

    int Execute(int argc, char *argv[]) override {
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
    TCreateVolumeCmd(TPortoAPI *api) : ICmd(api, "vcreate", 1, "-A|<path> [property=value...]",
        "create volume",
        "    -A        choose path automatically\n"
        ) {}

    int Execute(int argc, char *argv[]) override {
        std::map<std::string, std::string> properties;
        std::string path(argv[0]);

        if (path == "-A") {
            path = "";
        } else if (path[0] != '/') {
            std::cerr << "Volume path must be absolute" << std::endl;
            return EXIT_FAILURE;
        }

        for (int i = 1; i < argc; i++) {
            std::string arg(argv[i]);
            std::size_t sep = arg.find('=');
            if (sep == string::npos)
                properties[arg] = "";
            else
                properties[arg.substr(0, sep)] = arg.substr(sep + 1);
        }

        TVolumeDescription volume;
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
    TLinkVolumeCmd(TPortoAPI *api) : ICmd(api, "vlink", 1, "<path> [container]",
                    "link volume to container", "default container - current\n") {}

    int Execute(int argc, char *argv[]) override {
        int ret = Api->LinkVolume(argv[0], (argc > 1) ? argv[1] : "");
        if (ret)
            PrintError("Can't link volume");
        return ret;
    }
};

class TUnlinkVolumeCmd final : public ICmd {
public:
    TUnlinkVolumeCmd(TPortoAPI *api) : ICmd(api, "vunlink", 1, "<path> [container]",
                    "unlink volume from container",
                    "default container - current\n"
                    "removing last link deletes volume\n") {}

    int Execute(int argc, char *argv[]) override {
        int ret = Api->UnlinkVolume(argv[0], (argc > 1) ? argv[1] : "");
        if (ret)
            PrintError("Can't unlink volume");
        return ret;
    }
};

class TListVolumesCmd final : public ICmd {
    bool details = true;
    bool verbose = false;
    bool inodes = false;

public:
    TListVolumesCmd(TPortoAPI *api) : ICmd(api, "vlist", 0, "[-1|-i|-v] [volume]...",
        "list volumes",
        "    -1        list only paths\n"
        "    -i        list inode information\n"
        "    -v        list all properties\n"
        ) {}

    void ShowSizeProperty(TVolumeDescription &v, const char *p, int w, bool raw = false) {
      uint64_t val;

      if (!v.Properties.count(std::string(p)))
          std::cout << std::setw(w) << "-";
      else if (StringToUint64(v.Properties.at(std::string(p)), val))
          std::cout << std::setw(w) << "err";
      else if (raw)
          std::cout << std::setw(w) << val;
      else
          std::cout << std::setw(w) << StringWithUnit(val);
    }

    void ShowPercent(TVolumeDescription &v, const char *u, const char *a, int w) {
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

    void ShowVolume(TVolumeDescription &v) {
        if (!details) {
            std::cout << v.Path << std::endl;
        } else {
            std::cout << std::left << std::setw(40) << v.Path << std::right;
            if (v.Path.length() > 40)
                std::cout << std::endl << std::setw(40) << " ";
            if (inodes) {
                ShowSizeProperty(v, V_INODE_LIMIT, 8, true);
                ShowSizeProperty(v, V_INODE_USED, 8, true);
                ShowSizeProperty(v, V_INODE_AVAILABLE, 8, true);
                ShowPercent(v, V_INODE_USED, V_INODE_AVAILABLE, 5);
            } else {
                ShowSizeProperty(v, V_SPACE_LIMIT, 8);
                ShowSizeProperty(v, V_SPACE_USED, 8);
                ShowSizeProperty(v, V_SPACE_AVAILABLE, 8);
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

    int Execute(int argc, char *argv[]) override {
        int start = GetOpt(argc, argv, {
            { '1', false, [&](const char *arg) { details = false; } },
            { 'i', false, [&](const char *arg) { inodes = true; } },
            { 'v', false, [&](const char *arg) { verbose = true; details = false; } },
        });

        vector<TVolumeDescription> vlist;

        if (details) {
            std::cout << std::left << std::setw(40) << "Volume" << std::right;
            std::cout << std::setw(8) << "Limit";
            std::cout << std::setw(8) << "Used";
            std::cout << std::setw(8) << "Avail";
            std::cout << std::setw(5) << "Use%";
            std::cout << std::left << " Containers" << std::endl;
        }

        if (start == argc) {
          int ret = Api->ListVolumes(vlist);
          if (ret) {
              PrintError("Can't list volumes");
              return ret;
          }

          for (auto v : vlist)
              ShowVolume(v);
        } else {
            for (int i = start; i < argc; i++) {
                vlist.clear();
                int ret = Api->ListVolumes(argv[i], "", vlist);
                if (ret) {
                    PrintError(argv[i]);
                    continue;
                }
                for (auto v : vlist)
                    ShowVolume(v);
            }
        }

        return EXIT_SUCCESS;
    }
};

class TLayerCmd final : public ICmd {
public:
    TLayerCmd(TPortoAPI *api) : ICmd(api, "layer", 1,
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

    int Execute(int argc, char *argv[]) override {
        int ret = EXIT_SUCCESS;
        int start = GetOpt(argc, argv, {
            { 'I', false, [&](const char *arg) { import = true; } },
            { 'M', false, [&](const char *arg) { merge  = true; } },
            { 'R', false, [&](const char *arg) { remove = true; } },
            { 'F', false, [&](const char *arg) { flush  = true; } },
            { 'L', false, [&](const char *arg) { list   = true; } },
            { 'E', false, [&](const char *arg) { export_= true; } },
        });

        if (import) {
            if (argc < start + 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(argv[start], argv[start + 1]);
            if (ret)
                PrintError("Can't import layer");
        } else if (export_) {
            if (argc < start + 2)
                return EXIT_FAILURE;
            ret = Api->ExportLayer(argv[start], argv[start + 1]);
            if (ret)
                PrintError("Can't export layer");
        } else if (merge) {
            if (argc < start + 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(argv[start], argv[start + 1], true);
            if (ret)
                PrintError("Can't merge layer");
        } else if (remove) {
            if (argc < start + 1)
                return EXIT_FAILURE;

            for (int i = start; i < argc; i++) {
                ret = Api->RemoveLayer(argv[i]);
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
                for (auto l: layers)
                    (void)Api->RemoveLayer(l);
            }
        } else if (list) {
            std::vector<std::string> layers;
            ret = Api->ListLayers(layers);
            if (ret) {
                PrintError("Can't list layers");
            } else {
                for (auto l: layers)
                    std::cout << l << std::endl;
            }
        } else
            return EXIT_FAILURE;

        return ret;
    }
};

class TBuildCmd final : public ICmd {
    TVolumeBuilder VolumeBuilder;
    bool Cleanup = true;
    char *TmpFile = nullptr;

public:
    TBuildCmd(TPortoAPI *api) : ICmd(api, "build", 1,
            "[-C] [-o layer.tar] [-i NAT interface] [-E name=value]... "
            "<script> [top layer...] [bottom layer]",
            "build container image",
            "    -o layer.tar         save resulting upper layer\n"
            "    -E name=value        set environment variable\n"
            ), VolumeBuilder(api) {
        SetDieOnSignal(false);
    }

    ~TBuildCmd() {
        if (TmpFile) {
            TFile f(TmpFile);
            if (f.Exists())
                (void)f.Remove();
        }
    }

    int Execute(int argc, char *argv[]) override {
        TPath output = TPath(GetCwd()) / "layer.tar";
        std::vector<std::string> args;
        std::vector<std::string> env;

        int start = GetOpt(argc, argv, {
            { 'o', true, [&](const char *arg) { output = TPath(GetCwd()) / arg; } },
            { 'C', false, [&](const char *arg) { Cleanup = false; } },
            { 'E', true, [&](const char *arg) { env.push_back(arg); } },
        });

        if (!Cleanup) {
            args.push_back("-C");
            VolumeBuilder.NeedCleanup(false);
        }
        // don't use PTY in exec, use simple pipes so Ctrl-C works
        args.push_back("-T");

        TPath script = TPath(argv[start]).RealPath();
        if (!script.Exists()) {
            std::cerr << "Invalid script path " << script.ToString() << std::endl;
            return EXIT_FAILURE;
        }

        std::string name = "portctl-build-" + std::to_string(GetPid()) + "-" + script.BaseName();

        char *TmpFile = strdup("/tmp/portoctl-build-XXXXXX");
        int fd = mkstemp(TmpFile);
        if (fd < 0) {
            perror("mkstemp");
            return EXIT_FAILURE;
        }
        close(fd);

        // make sure apt-get doesn't ask any questions
        env.push_back("DEBIAN_FRONTEND=noninteractive");

        std::vector<std::string> bind;

        // mount host selinux into container (for fedora)
        TPath selinux("/sys/fs/selinux");
        if (selinux.Exists())
            bind.push_back("/sys/fs/selinux /sys/fs/selinux ro");

        std::vector<std::string> layers;
        for (auto i = start + 1; i < argc; i++)
            layers.push_back(argv[i]);

        args.push_back(name);

        if (layers.size()) {
            // custom layer

            TError error = VolumeBuilder.Prepare(layers);
            if (error) {
                std::cerr << "Can't create volume: " << error << std::endl;
                return EXIT_FAILURE;
            }
            bind.push_back(script.ToString() + " /tmp/script ro");

            args.push_back("root=" + VolumeBuilder.GetVolumePath());
            args.push_back("command=/bin/bash -e -x -c '. /tmp/script'");
        } else {
            // base layer

            TError error = VolumeBuilder.Prepare();
            if (error) {
                std::cerr << "Can't create volume: " << error << std::endl;
                return EXIT_FAILURE;
            }

            auto volume = VolumeBuilder.GetVolumePath();

            bind.push_back("/tmp /tmp rw");
            bind.push_back(volume + " " + volume + " rw");

            args.push_back("root_readonly=true");
            args.push_back("cwd=" + volume);
            args.push_back("command=/bin/bash -e -x -c '. " + script.ToString() + " " + volume + "'");
        }

        args.push_back("bind=" + CommaSeparatedList(bind, ";"));
        args.push_back("bind_dns=false");
        args.push_back("user=root");
        args.push_back("group=root");
        args.push_back("env=" + CommaSeparatedList(env, ";"));

        int ret = RunCmd<TExecCmd>(args);
        if (ret)
            return ret;

        TError error = VolumeBuilder.ExportLayer(output);
        if (error) {
            std::cerr << "Can't export layer:" << error << std::endl;
            return EXIT_FAILURE;
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
    RegisterCommand(new TSortCmd(&api));
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
    RegisterCommand(new TGcCmd(&api));
    RegisterCommand(new TFindCmd(&api));
    RegisterCommand(new TWaitCmd(&api));

    RegisterCommand(new TCreateVolumeCmd(&api));
    RegisterCommand(new TLinkVolumeCmd(&api));
    RegisterCommand(new TUnlinkVolumeCmd(&api));
    RegisterCommand(new TListVolumesCmd(&api));

    RegisterCommand(new TLayerCmd(&api));
    RegisterCommand(new TBuildCmd(&api));

    TLogger::DisableLog();

    int ret = HandleCommand(&api, argc, argv);
    if (ret < 0) {
        ResetAllSignalHandlers();
        raise(-ret);
        return EXIT_FAILURE;
    } else {
        return ret;
    }
};
