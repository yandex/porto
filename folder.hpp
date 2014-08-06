#ifndef __FOLDER_HPP__
#define __FOLDER_HPP__

#include <string>
#include <vector>

#include "file.hpp"

class TFolder {
    std::string path;

public:
    TFolder(std::string path);
    TFolder(TFile file);

    bool Exists();
    TError Create(mode_t mode = 0x755);
    TError Remove(bool recursive = false);
    TError Rename(std::string newname);

    TError Items(const TFile::EFileType type, std::vector<std::string> &list);
    TError Subfolders(std::vector<std::string> &list);
};

#endif
