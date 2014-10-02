#ifndef __FOLDER_HPP__
#define __FOLDER_HPP__

#include <string>
#include <vector>

#include "file.hpp"

class TFolder {
    const std::string Path;
    const bool Tmp;

public:
    TFolder(const std::string &path, bool tmp = false) : Path(path), Tmp(tmp) {}
    TFolder(TFile file, bool tmp = false) : Path(file.GetPath()), Tmp(tmp) {}
    ~TFolder();

    const std::string &GetPath() const;
    bool Exists() const;
    TError Create(mode_t mode = 0755, bool recursive = false) const;
    TError Remove(bool recursive = false) const;

    TError Items(const TFile::EFileType type, std::vector<std::string> &list) const;
    TError Subfolders(std::vector<std::string> &list) const;
    TError Chown(const std::string &user, const std::string &group) const;
};

#endif
