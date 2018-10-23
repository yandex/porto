#include <sstream>

#include "env.hpp"
#include "util/log.hpp"

extern "C" {
#include <sys/resource.h>
}

void TEnv::ClearEnv() {
    Vars.clear();
    Environ.clear();
}

bool TEnv::GetEnv(const std::string &name, std::string &value) const {
    for (const auto &var: Vars) {
        if (var.Set && var.Name == name) {
            value = var.Value;
            return true;
        }
    }
    return false;
}

TError TEnv::SetEnv(const std::string &name, const std::string &value,
                    bool overwrite /* true */, bool lock /* false */) {
    for (auto &var: Vars) {
        if (var.Name != name)
            continue;
        if (!overwrite)
            return TError(EError::InvalidValue, "variable " + name + " already set");
        if (var.Locked && value != var.Value) {
            L("Variable {} locked to {}, value {} is ignored", name, var.Value, value);
            return OK;
        }
        var.Value = value;
        var.Set = true;
        var.Locked = lock;
        var.Overwritten = overwrite;
        return OK;
    }
    Vars.push_back({name, value, true, lock, overwrite, ""});
    return OK;
}

TError TEnv::UnsetEnv(const std::string &name, bool overwrite /* true */) {
    for (auto &var: Vars) {
        if (var.Name != name)
            continue;
        if (!overwrite && var.Set)
            return TError(EError::InvalidValue, "variable " + name + " already set");
        if (var.Locked && var.Set) {
            L("Variable {} locked to {}, unset is ignored", name, var.Value);
            return OK;
        }
        var.Value = "";
        var.Set = false;
        var.Overwritten = overwrite;
        return OK;
    }
    Vars.push_back({name, "", false, false, overwrite, ""});
    return OK;
}

TError TEnv::Parse(const std::string &cfg, bool overwrite) {
    for (auto &str: SplitEscapedString(cfg, ';')) {
        auto sep = str.find('=');
        TError error;

        if (sep == std::string::npos)
            error = UnsetEnv(str, overwrite);
        else
            error = SetEnv(str.substr(0, sep),
                           str.substr(sep + 1), overwrite);
        if (error && overwrite)
            return error;
    }
    return OK;
}

void TEnv::Format(std::string &cfg) const {
    TTuple tuple;
    for (const auto &var: Vars) {
        if (var.Set)
            tuple.push_back(var.Name + "=" + var.Value);
        else
            tuple.push_back(var.Name);
    }
    cfg = MergeEscapeStrings(tuple, ';');
}

TError TEnv::Apply() const {
    clearenv();
    for (auto &var: Vars) {
        if (var.Set && setenv(var.Name.c_str(), var.Value.c_str(), 1))
            return TError::System("setenv");
    }
    return OK;
}

char **TEnv::Envp() {
    int i = 0;

    Environ.resize(Vars.size() + 1);
    for (auto &var: Vars) {
        if (var.Set) {
            var.Data = var.Name + "=" + var.Value;
            Environ[i++] = (char *)var.Data.c_str();
        }
    }
    Environ[i] = nullptr;

    return (char **)Environ.data();
}

/* <type>: <soft>|unlimited [hard] */
TError TUlimitResource::Parse(const std::string &str) {
    auto col = str.find(':');
    TError error;

    if (col == std::string::npos)
        return TError(EError::InvalidValue, "Invalid ulimit: {}", str);

    auto name = StringTrim(str.substr(0, col));
    Type = TUlimit::GetType(name);
    if (Type < 0)
        return TError(EError::InvalidValue, "Invalid ulimit: {}", str);

    auto arg = StringTrim(str.substr(col + 1));
    auto sep = arg.find(' ');
    auto val = arg.substr(0, sep);
    if (val == "unlimited" || val == "unlim" || val == "inf" || val == "-1") {
        Soft = RLIM_INFINITY;
    } else {
        error = StringToSize(val, Soft);
        if (error)
            return TError(error, "Invalid ulimit: {}", str);
    }

    val = sep == std::string::npos ? "" : StringTrim(arg.substr(sep));
    if (val == "") {
        Hard = Soft;
    } else if (val == "unlimited" || val == "unlim" || val == "inf" || val == "-1") {
        Hard = RLIM_INFINITY;
    } else {
        error = StringToSize(val, Hard);
        if (error)
            return TError(error, "Invalid ulimit: {}", str);
    }

    return OK;
}

int TUlimit::GetType(const std::string &name) {
    static const std::map<std::string, int> types = {
        { "as", RLIMIT_AS },
        { "core", RLIMIT_CORE },
        { "cpu", RLIMIT_CPU },
        { "data", RLIMIT_DATA },
        { "fsize", RLIMIT_FSIZE },
        { "locks", RLIMIT_LOCKS },
        { "memlock", RLIMIT_MEMLOCK },
        { "msgqueue", RLIMIT_MSGQUEUE },
        { "nice", RLIMIT_NICE },
        { "nofile", RLIMIT_NOFILE },
        { "nproc", RLIMIT_NPROC },
        { "rss", RLIMIT_RSS },
        { "rtprio", RLIMIT_RTPRIO },
        { "rttime", RLIMIT_RTTIME },
        { "sigpending", RLIMIT_SIGPENDING },
        { "stack", RLIMIT_STACK },
    };
    auto idx = types.find(name);
    return idx == types.end() ? -1 : idx->second;
}

std::string TUlimit::GetName(int type) {
    switch (type) {
    case RLIMIT_AS:
        return "as";
    case RLIMIT_CORE:
        return "core";
    case RLIMIT_CPU:
        return "cpu";
    case RLIMIT_DATA:
        return "data";
    case RLIMIT_FSIZE:
        return "fsize";
    case RLIMIT_LOCKS:
        return "locks";
    case RLIMIT_MEMLOCK:
        return "memlock";
    case RLIMIT_MSGQUEUE:
        return "msgqueue";
    case RLIMIT_NICE:
        return "nice";
    case RLIMIT_NOFILE:
        return "nofile";
    case RLIMIT_NPROC:
        return "nproc";
    case RLIMIT_RSS:
        return "rss";
    case RLIMIT_RTPRIO:
        return "rtprio";
    case RLIMIT_RTTIME:
        return "rttime";
    case RLIMIT_SIGPENDING:
        return "sigpending";
    case RLIMIT_STACK:
        return "stack";
    default:
        return "???";
    }
}

TError TUlimit::Parse(const std::string &str) {
    std::istringstream ss(str);
    std::string lim;
    TError error;

    while(std::getline(ss, lim, ';')) {
        lim = StringTrim(lim);
        if (!lim.size())
            continue;

        TUlimitResource res;
        error = res.Parse(lim);
        if (error)
            return error;

        Set(res.Type, res.Soft, res.Hard, true);
    }

    return OK;
}

std::string TUlimit::Format(uint64_t val) {
    return val < RLIM_INFINITY ? std::to_string(val) : "unlimited";
}

std::string TUlimit::Format() const {
    std::string str;
    for (auto &res: Resources)
        str += fmt::format("{}: {} {}; ",
                           TUlimit::GetName(res.Type),
                           TUlimit::Format(res.Soft),
                           TUlimit::Format(res.Hard));
    return str;
}

TError TUlimit::Load(pid_t pid) {
    Clear();
    for (int type = 0; type < RLIM_NLIMITS; type++) {
        struct rlimit lim;
        if (prlimit(pid, (enum __rlimit_resource)type, nullptr, &lim))
            return TError::System("prlimit {} {}", pid, TUlimit::GetName(type));
        Set(type, lim.rlim_cur, lim.rlim_max, true);
    }
    return OK;
}

TError TUlimit::Apply(pid_t pid) const {
    struct rlimit lim;
    for (auto &res: Resources) {
        lim.rlim_cur = res.Soft < RLIM_INFINITY ? res.Soft : RLIM_INFINITY;
        lim.rlim_max = res.Hard < RLIM_INFINITY ? res.Hard : RLIM_INFINITY;
        if (prlimit(pid, (enum __rlimit_resource)res.Type, &lim, nullptr))
            return TError::System("prlimit {} {} {} {}", pid, TUlimit::GetName(res.Type), res.Soft, res.Hard);
    }
    return OK;
}

void TUlimit::Set(int type, uint64_t soft, uint64_t hard, bool overwrite) {
    if (type < 0 || type >= RLIM_NLIMITS)
        return;
    bool found = false;
    for (auto &res: Resources) {
        if (res.Type == type) {
            found = true;
            if (overwrite)
                res = {type, soft, hard, overwrite};
            break;
        }
    }
    if (!found)
        Resources.push_back({type, soft, hard, overwrite});
}

void TUlimit::Merge(const TUlimit &ulimit, bool overwrite) {
    for (const auto &res: ulimit.Resources)
        Set(res.Type, res.Soft, res.Hard, overwrite);
}
