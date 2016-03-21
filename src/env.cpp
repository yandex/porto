#include "env.hpp"
#include "util/log.hpp"

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
        if (var.Locked) {
            L_WRN() << "Variable " << name << " locked to " << var.Value
                    << ", value " << value << " is ignored" << std::endl;
            return TError::Success();
        }
        var.Value = value;
        var.Set = true;
        var.Locked = lock;
        return TError::Success();
    }
    Vars.push_back({name, value, true, lock, ""});
    return TError::Success();
}

TError TEnv::UnsetEnv(const std::string &name, bool overwrite /* true */) {
    for (auto &var: Vars) {
        if (var.Name != name)
            continue;
        if (!overwrite && var.Set)
            return TError(EError::InvalidValue, "variable " + name + " already set");
        if (var.Locked) {
            L_WRN() << "Variable " << name << " locked to " << var.Value
                    << ", unset is ignored" << std::endl;
            return TError::Success();
        }
        var.Value = "";
        var.Set = false;
        return TError::Success();
    }
    Vars.push_back({name, "", false, false, ""});
    return TError::Success();
}

TError TEnv::Parse(const std::vector<std::string> &cfg, bool overwrite) {
    for (auto &str: cfg) {
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
    return TError::Success();
}

void TEnv::Format(std::vector<std::string> &cfg) const {
    cfg.clear();
    for (const auto &var: Vars) {
        if (var.Set)
            cfg.push_back(var.Name + "=" + var.Value);
        else
            cfg.push_back(var.Name);
    }
}

TError TEnv::Apply() const {
    clearenv();
    for (auto &var: Vars) {
        if (var.Set && setenv(var.Name.c_str(), var.Value.c_str(), 1))
            return TError(EError::Unknown, errno, "setenv");
    }
    return TError::Success();
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
