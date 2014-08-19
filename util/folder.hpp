#ifndef __FOLDER_HPP__
#define __FOLDER_HPP__

#include <string>
#include <vector>

#include "file.hpp"

class TFolder {
    const std::string path;

public:
    TFolder(const std::string &path) : path(path) {}
    TFolder(TFile file) : path(file.Path()) {}

    bool Exists();
    TError Create(mode_t mode = 0755);
    TError Remove(bool recursive = false);
    TError Rename(const std::string &newname);

    TError Items(const TFile::EFileType type, std::vector<std::string> &list);
    TError Subfolders(std::vector<std::string> &list);
};

#endif
