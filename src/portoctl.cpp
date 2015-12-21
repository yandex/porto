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
    std::vector<std::string> ImportedLayers;
    bool Cleanup = true;
    int LayerIdx = 0;

    TError GetLastError() {
        int error;
        std::string msg;
        Api->GetLastError(error, msg);
        return TError((EError)error, msg);
    }

    TError ImportLayer(const TPath &path, std::string &id) {
        id = "portoctl-" + std::to_string(GetPid()) + "-" +
             std::to_string(LayerIdx++) + "-" + path.BaseName();
        std::cout << "Importing layer " << path << " as " << id << std::endl;
        int ret = Api->ImportLayer(id, path.ToString());
        if (ret)
            return GetLastError();
        ImportedLayers.push_back(id);
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

            for (auto id : ImportedLayers) {
                std::cout << "Removing layer " << id << std::endl;
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
        std::vector<std::string> known;

        if (layers.empty())
            return CreatePlainVolume(Volume);

        if (Api->ListLayers(known))
            return GetLastError();

        for (auto layer : layers) {
            layer = StringTrim(layer);

            if (layer.empty())
                continue;

            if (std::find(known.begin(), known.end(), layer) != known.end()) {
                Layers.push_back(layer);
                continue;
            }

            auto path = TPath(layer).RealPath();

            if (path.IsDirectory()) {
                Layers.push_back(path.ToString());
                continue;
            }

            std::string id;
            TError error = ImportLayer(path, id);
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
    TCreateCmd(TPortoAPI *api) : ICmd(api, "create", 1, "<container1> [container2...]", "create container") {}

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
    TGetPropertyCmd(TPortoAPI *api) : ICmd(api, "pget", 2, "[-k] <container> <property> [property...]", "get raw container property") {}

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
    TSetPropertyCmd(TPortoAPI *api) : ICmd(api, "set", 3, "<container> <property>", "set container property") {}

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
    TGetDataCmd(TPortoAPI *api) : ICmd(api, "dget", 2, "[-k] <container> <data> [data...]", "get raw container data") {}

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
    TStartCmd(TPortoAPI *api) : ICmd(api, "start", 1, "<container1> [container2...]", "start container") {}

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
    TKillCmd(TPortoAPI *api) : ICmd(api, "kill", 1, "<container> [signal]", "send signal to container") {}

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
    TStopCmd(TPortoAPI *api) : ICmd(api, "stop", 1, "<container1> [container2...]", "stop container") {}

    int Execute(TCommandEnviroment *env) final override {
        for (const auto &arg : env->GetArgs()) {
            int ret = Api->Stop(arg);
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
    TPauseCmd(TPortoAPI *api) : ICmd(api, "pause", 1, "<container> [name...]", "pause container") {}

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
    TResumeCmd(TPortoAPI *api) : ICmd(api, "resume", 1, "<container1> [container2...]", "resume container") {}

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
    TGetCmd(TPortoAPI *api) : ICmd(api, "get", 1, "<container> <variable> [variable...]", "get container property or data") {}

    int Execute(TCommandEnviroment *env) final override {
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

        std::map<std::string, std::map<std::string, TPortoGetResponse>> result;
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
    TEnterCmd(TPortoAPI *api) : ICmd(api, "enter", 1,
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

        vector<shared_ptr<TMount>> mounts;
        error = TMount::Snapshot(mounts);
        if (error)
            return error;

        for (auto &mount : mounts) {
            auto data = mount->GetData();
            bool found = true;
            for (auto &ss : subsystems) {
                if (std::find(data.begin(), data.end(), ss) == data.end()) {
                    found = false;
                    break;
                }
            }

            if (found) {
                root = mount->GetMountpoint();
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

                TFile f(root / cg.second / "cgroup.procs");
                error = f.AppendString(std::to_string(GetPid()));
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
    TVolumeBuilder VolumeBuilder;
public:
    TRunCmd(TPortoAPI *api) : ICmd(api, "run", 2,
            "[-L layer]... <container> [properties]",
            "create and start container with given properties",
            "    -L layer|dir|tarball        add lower layer (-L top ... -L bottom)\n"
            ), VolumeBuilder(api) {}

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

    int Execute(TCommandEnviroment *env) final override {
        std::vector<std::string> layers;
        const auto &args = env->GetOpts({
            { 'L', true, [&](const char *arg) { layers.push_back(arg); } },
        });

        const string &containerName = args[0];
        std::vector<std::pair<std::string, std::string>> properties;

        int ret;

        for (size_t i = 1; i < args.size(); ++i) {
            string key, val;
            ret = Parser(args[i], key, val);
            if (ret)
                return ret;

            properties.emplace_back(std::make_pair(key, val));
        }

        if (!layers.empty()) {
            TError error = VolumeBuilder.Prepare(layers);
            if (error) {
                std::cerr << "Can't create volume: " << error << std::endl;
                return EXIT_FAILURE;
            }

            properties.emplace_back(std::make_pair("root", VolumeBuilder.GetVolumePath()));
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
    TExecCmd(TPortoAPI *api) : ICmd(api, "exec", 2,
        "[-C] [-T] [-L layer]... <container> command=<command> [properties]",
        "create pty, execute and wait for command in container",
        "    -L layer|dir|tarball        add lower layer (-L top ... -L bottom)\n"
        ), VolumeBuilder(api) {
        SetDieOnSignal(false);
    }

    ~TExecCmd() {
        if (Cleanup && !containerName.empty()) {
            TPortoAPI api(config().rpc_sock().file().path());
            (void)api.Destroy(containerName);
        }
        if (TmpDir)
            (void)TPath(TmpDir).RemoveAll();
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

    bool MoveData(int from, int to) {
        char buf[256];
        int ret;

        ret = read(from, buf, sizeof(buf));
        if (ret > 0) {
            if (ret == 2 && buf[0] == '^' && buf[1] == 'C')
                return true;
            if (write(to, buf, ret) != ret)
                std::cerr << "Partial write to " << to << std::endl;
        }

        return false;
    }

    int OpenTemp(const std::string &path, int flags) {
        int fd = open(path.c_str(), flags);
        if (fd < 0) {
            TError error(EError::Unknown, errno, "open()");
            PrintError(error, "Can't open temporary file " + path);
        }

        return fd;
    }

    int Execute(TCommandEnviroment *environment) final override {
        bool hasTty = isatty(STDIN_FILENO);
        std::vector<std::string> args;
        std::string env;
        std::vector<std::string> layers;

        const auto &argv = environment->GetOpts({
            { 'C', false, [&](const char *arg) { Cleanup = false; } },
            { 'T', false, [&](const char *arg) { hasTty = false; } },
            { 'L', true, [&](const char *arg) { layers.push_back(arg); } },
        });
        VolumeBuilder.NeedCleanup(Cleanup);

        if (hasTty)
            env = std::string("TERM=") + getenv("TERM");

        args.push_back(argv[0]);
        for (size_t i = 1; i < argv.size(); ++i)
            if (argv[i].find("env=", 0, 4) != std::string::npos)
                env += "; " + argv[i].substr(4);
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

            args.push_back("stdin_path=" + stdinPath);
            args.push_back("stdout_path=" + stdoutPath);
            args.push_back("stderr_path=" + stderrPath);

            args.push_back("stdin_type=pty");
            args.push_back("stdout_type=pty");
            args.push_back("stderr_type=pty");

            if (env.length())
                args.push_back("env=" + env);

            int ret = RunCmd<TRunCmd>(args, environment);
            if (ret)
                return ret;
        } else {
            TmpDir = mkdtemp(strdup("/tmp/portoctl-exec-XXXXXX"));
            if (!TmpDir) {
                TError error(EError::Unknown, errno, "mkdtemp()");
                PrintError(error, "Can't create temporary directory");
                return EXIT_FAILURE;
            }

            stdinPath = string(TmpDir) + "/stdin";
            stdoutPath = string(TmpDir) + "/stdout";
            stderrPath = string(TmpDir) + "/stderr";

            args.push_back("stdin_path=" + stdinPath);
            args.push_back("stdout_path=" + stdoutPath);
            args.push_back("stderr_path=" + stderrPath);

            args.push_back("stdin_type=fifo");
            args.push_back("stdout_type=fifo");
            args.push_back("stderr_type=fifo");

            if (env.length())
                args.push_back("env=" + env);

            int ret = RunCmd<TRunCmd>(args, environment);
            if (ret)
                return ret;

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

        containerName = argv[0];

        bool hangup = false;
        while (!hangup) {
            if (GotSignal())
                return EXIT_FAILURE;

            int ret = poll(fds.data(), fds.size(), -1);
            if (ret < 0)
                break;

            for (size_t i = 0; i < fds.size(); i++) {
                if (fds[i].revents & POLLIN) {
                    int dest;

                    if (fds[i].fd == STDIN_FILENO)
                        dest = stdinFd;
                    else if (fds[i].fd == stdoutFd)
                        dest = STDOUT_FILENO;
                    else if (fds[i].fd == stderrFd)
                        dest = STDERR_FILENO;
                    else
                        continue;

                    if (MoveData(fds[i].fd, dest))
                        return EXIT_FAILURE;
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
        int ret = Api->Wait({ containerName }, tmp);
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
    TFindCmd(TPortoAPI *api) : ICmd(api, "find", 1, "", "find container for given process id") {}

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
    TWaitCmd(TPortoAPI *api) : ICmd(api, "wait", 1, "<container1> [container2] ...", "wait for listed containers") {}

    int Execute(TCommandEnviroment *env) final override {
        int timeout = -1;
        const auto &containers = env->GetOpts({
            { 't', true, [&](const char *arg) { timeout = std::stoi(arg); } },
        });

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
    TSortCmd(TPortoAPI *api) : ICmd(api, "sort", 0, "[sort-by]", "print containers sorted by resource usage") {}

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
    TCreateVolumeCmd(TPortoAPI *api) : ICmd(api, "vcreate", 1, "-A|<path> [property=value...]",
        "create volume",
        "    -A        choose path automatically\n"
        ) {}

    int Execute(TCommandEnviroment *env) final override {
        std::map<std::string, std::string> properties;
        const auto &args = env->GetArgs();
        std::string path = args[0];

        if (path == "-A") {
            path = "";
        } else if (path[0] != '/') {
            std::cerr << "Volume path must be absolute" << std::endl;
            return EXIT_FAILURE;
        }

        for (size_t i = 1; i < args.size(); i++) {
            const std::string &arg = args[i];
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

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        int ret = Api->LinkVolume(args[0], (args.size() > 1) ? args[1] : "");
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

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetArgs();
        int ret = Api->UnlinkVolume(args[0], (args.size() > 1) ? args[1] : "");
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
          std::cout << std::setw(w) << StringFormatSize(val);
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

    int Execute(TCommandEnviroment *env) final override {
        const auto &args = env->GetOpts({
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
                vlist.clear();
                int ret = Api->ListVolumes(arg, "", vlist);
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

        if (import) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], args[1]);
            if (ret)
                PrintError("Can't import layer");
        } else if (export_) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ExportLayer(args[0], args[1]);
            if (ret)
                PrintError("Can't export layer");
        } else if (merge) {
            if (args.size() < 2)
                return EXIT_FAILURE;
            ret = Api->ImportLayer(args[0], args[1], true);
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
    TVolumeBuilder VolumeBuilder;
    bool Cleanup = true;

public:
    TBuildCmd(TPortoAPI *api) : ICmd(api, "build", 0,
            "[-k] [-L layer]... -o layer.tar [-E name=value]... "
            "[-B bootstrap] -S script [properties]...",
            "build container image",
            "    -L layer|dir|tarball       add lower layer (-L top ... -L bottom)\n"
            "    -o layer.tar               save resulting upper layer\n"
            "    -E name=value              set environment variable\n"
            "    -B bootstrap               bash script runs outside (with cwd=volume)\n"
            "    -S script                  bash script runs inside (with root=volume)\n"
            "    -k                         keep volume and container\n"
            ), VolumeBuilder(api) {
        SetDieOnSignal(false);
    }

    ~TBuildCmd() { }

    int Execute(TCommandEnviroment *environment) final override {
        std::string name = "portoctl-build-" + std::to_string(GetPid());
        TPath output;
        std::vector<std::string> env;
        std::vector<std::string> layers;
        TPath bootstrap, script;
        const auto &opts = environment->GetOpts({
            { 'L', true, [&](const char *arg) { layers.push_back(arg); } },
            { 'o', true, [&](const char *arg) { output =  TPath(arg).AbsolutePath(); } },
            { 'E', true, [&](const char *arg) { env.push_back(arg); } },
            { 'B', true, [&](const char *arg) { bootstrap = TPath(arg).RealPath(); } },
            { 'S', true, [&](const char *arg) { script = TPath(arg).RealPath(); } },
            { 'k', false, [&](const char *arg) { Cleanup = false; } },
        });

        if (output.IsEmpty()) {
            std::cerr << "No output file specified" << std::endl;
            PrintUsage();
            return EXIT_FAILURE;
        }

        if (output.Exists()) {
            std::cerr << "Output file " << output << " already exists" << std::endl;
            PrintUsage();
            return EXIT_FAILURE;
        }

        if (!output.DirName().Exists()) {
            std::cerr << "Output directory " << output.DirName() << " not exists" << std::endl;
            PrintUsage();
            return EXIT_FAILURE;
        }

        if (script.IsEmpty()) {
            std::cerr << "Not script specified" << std::endl;
            PrintUsage();
            return EXIT_FAILURE;
        }

        if (!script.Exists()) {
            std::cerr << "Script " << script << " not exists" << std::endl;
            return EXIT_FAILURE;
        }

        if (!bootstrap.IsEmpty() && !bootstrap.Exists()) {
            std::cerr << "Bootstrap " << bootstrap << " not exists" << std::endl;
            return EXIT_FAILURE;
        }

        TError error = VolumeBuilder.Prepare(layers);
        if (error) {
            std::cerr << "Cannot create volume: " << error << std::endl;
            return EXIT_FAILURE;
        }
        VolumeBuilder.NeedCleanup(Cleanup);

        auto volume = VolumeBuilder.GetVolumePath();

        if (!bootstrap.IsEmpty()) {
            std::vector<std::string> args;
            std::vector<std::string> bind;

            /* don't use PTY in exec, use simple pipes so Ctrl-C works FIXME */
            args.push_back("-T");

            args.push_back(name + "-" + bootstrap.BaseName());

            args.push_back("isolate=true");
            args.push_back("env=" + CommaSeparatedList(env, ";"));

            /* give write access only to volume and /tmp */
            args.push_back("root_readonly=true");
            bind.push_back("/tmp /tmp rw");
            bind.push_back(volume + " " + volume + " rw");
            args.push_back("bind=" + CommaSeparatedList(bind, ";"));

            /* add properies from commandline */
            for (auto opt : opts)
                args.push_back(opt);

            args.push_back("cwd=" + volume);
            args.push_back("command=/bin/bash -e -x -c '. " + bootstrap.ToString() + "'");

            std::cout << "Starting bootstrap: exec " << CommaSeparatedList(args, "  ") << std::endl;

            int ret = RunCmd<TExecCmd>(args, environment);
            if (ret) {
                std::cout << "Bootstrap failed with exitcode: " << ret << std::endl;
                return ret;
            }
        }

        if (!script.IsEmpty()) {
            std::vector<std::string> args;
            std::vector<std::string> bind;

            args.push_back(name);
            args.push_back("virt_mode=os");

            /* add properies from commandline */
            for (auto opt : opts)
                args.push_back(opt);

            /* Mount host selinux into container (for fedora) */
            TPath selinux("/sys/fs/selinux");
            if (selinux.Exists())
                bind.push_back("/sys/fs/selinux /sys/fs/selinux ro");

            bind.push_back(script.ToString() + " /" + name + " ro");
            args.push_back("bind=" + CommaSeparatedList(bind, ";"));
            args.push_back("root=" + volume);

            std::cout << "Creating container: run " << CommaSeparatedList(args, "  ") << std::endl;

            int ret = RunCmd<TRunCmd>(args, environment);
            if (ret)
                return ret;

            args.clear();
            args.push_back("-T");
            args.push_back(name + "/" + script.BaseName());
            args.push_back("isolate=false");
            args.push_back("env=" + CommaSeparatedList(env, ";"));
            args.push_back("command=/bin/bash -e -x -c '. /" + name + "'");

            std::cout << "Starting script: exec " << CommaSeparatedList(args, "  ") << std::endl;

            int ret2 = RunCmd<TExecCmd>(args, environment);
            if (ret2)
                std::cout << "Script failed with exitcode: " << ret2 << std::endl;

            if (Cleanup) {
                std::cout << "Destroying container: destroy " + name << std::endl;

                args.clear();
                args.push_back(name);
                ret = RunCmd<TDestroyCmd>(args, environment);
                if (ret)
                    return ret;
            }

            if (ret2)
                return ret2;
        }

        TPath(volume + "/" + name).Unlink();

        std::cout << "Exporting upper layer into " << output.ToString() << std::endl;

        error = VolumeBuilder.ExportLayer(output);
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
    handler.RegisterCommand<TGcCmd>();
    handler.RegisterCommand<TFindCmd>();
    handler.RegisterCommand<TWaitCmd>();

    handler.RegisterCommand<TCreateVolumeCmd>();
    handler.RegisterCommand<TLinkVolumeCmd>();
    handler.RegisterCommand<TUnlinkVolumeCmd>();
    handler.RegisterCommand<TListVolumesCmd>();

    handler.RegisterCommand<TLayerCmd>();
    handler.RegisterCommand<TBuildCmd>();

    TLogger::DisableLog();

    int ret = handler.HandleCommand(argc, argv);
    if (ret < 0) {
        ResetAllSignalHandlers();
        raise(-ret);
        return EXIT_FAILURE;
    } else {
        return ret;
    }
};
