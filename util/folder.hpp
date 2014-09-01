#ifndef __FOLDER_HPP__
#define __FOLDER_HPP__

#include <string>
#include <vector>

#include "file.hpp"

class TFolder {
    const std::string Path;

public:
    TFolder(const std::string &path) : Path(path) {}
    TFolder(TFile file) : Path(file.GetPath()) {}

    bool Exists() const;
    TError Create(mode_t mode = 0755, bool recursive = false) const;
    TError Remove(bool recursive = false) const;

    TError Items(const TFile::EFileType type, std::vector<std::string> &list) const;
    TError Subfolders(std::vector<std::string> &list) const;
};

#endif
