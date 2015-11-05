#pragma once

#include <string>
#include <vector>

#include "util/path.hpp"
#include "util/cred.hpp"
#include "util/file.hpp"

class TFolder {
    const TPath Path;
    const bool Tmp;

public:
    TFolder(const TPath &path, bool tmp = false) : Path(path), Tmp(tmp) {}
    TFolder(const TFile &file, bool tmp = false) : Path(file.GetPath()), Tmp(tmp) {}
    ~TFolder();

    const TPath &GetPath() const { return Path; }
    bool Exists() const { return Path.Exists(); }

    TError Create(mode_t mode = 0755, bool recursive = false) const;
    TError Remove(bool recursive = false, bool silent = true) const;

    TError Items(const EFileType type, std::vector<std::string> &list) const;
    TError Subfolders(std::vector<std::string> &list) const;
};

void RemoveIf(const TPath &path,
              EFileType type,
              std::function<bool(const std::string &name, const TPath &path)> f);
