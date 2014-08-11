#include "folder.hpp"
#include "log.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <unistd.h>

#include <unordered_map>

using namespace std;

TFolder::TFolder(const string &path) : path(path) {}
TFolder::TFolder(TFile file) : path(file.Path()) {}

TError TFolder::Create(mode_t mode) {
    int ret = mkdir(path.c_str(), mode);

    TLogger::LogAction("mkdir " + path, ret, errno);

    return TError(errno);
}

TError TFolder::Remove(bool recursive) {
    if (recursive) {
        vector<string> items;
        TError error = Items(TFile::Any, items);
        if (error)
            return error;

        for (auto f : items) {
            TFile child(f);

            if (child.Type() == TFile::Directory)
                TFolder(f).Remove(recursive);
            else
                child.Remove();
        }
    }

    int ret = rmdir(path.c_str());

    TLogger::LogAction("rmdir " + path, ret, errno);

    return TError(errno);
}

TError TFolder::Rename(const std::string &newname) {
    //TODO
    return TError("TODO");
}

bool TFolder::Exists() {
    struct stat st;

    int ret = stat(path.c_str(), &st);

    if (ret == 0 && S_ISDIR(st.st_mode))
        return true;
    else if (ret && errno == ENOENT)
        return false;
    else
        throw "Cannot stat folder: " + path;
}

TError TFolder::Subfolders(std::vector<std::string> &list) {
    return Items(TFile::Directory, list);
}

TError TFolder::Items(const TFile::EFileType type, std::vector<std::string> &list) {
    DIR *dirp;
    struct dirent dp, *res;

    dirp = opendir(path.c_str());
    if (!dirp)
        return TError("Cannot open folder " + path);

    while (!readdir_r(dirp, &dp, &res) && res != nullptr) {
        if (!strcmp(".", res->d_name) || !strcmp ("..", res->d_name))
            continue;

        static unordered_map<unsigned char, TFile::EFileType> d_type_to_type =
            {{DT_UNKNOWN, TFile::Unknown},
             {DT_FIFO, TFile::Fifo},
             {DT_CHR, TFile::Character},
             {DT_DIR, TFile::Directory},
             {DT_BLK, TFile::Block},
             {DT_REG, TFile::Regular},
             {DT_LNK, TFile::Link},
             {DT_SOCK, TFile::Socket}};

        if (type == TFile::Any || type == d_type_to_type[res->d_type])
            list.push_back(string(res->d_name));
    }

    closedir(dirp);
    return TError();
}
