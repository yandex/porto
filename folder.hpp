#ifndef __FOLDER_H__
#define __FOLDER_H__

#include <string>
#include <vector>

#include "file.hpp"

class TFolder {
    std::string path;

public:
    TFolder(std::string path);
    TFolder(TFile file);

    bool Exists();
    void Create(mode_t mode = 0x755);
    void Remove(bool recursive = false);
    void Rename(std::string newname);

    std::vector<std::string> Items(TFile::EFileType type);
    std::vector<std::string> Subfolders();
};

#endif
