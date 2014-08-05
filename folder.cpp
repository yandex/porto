#include "folder.hpp"
#include "log.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <unistd.h>

#include <unordered_map>

using namespace std;

TFolder::TFolder(string path) : path(path) {}
TFolder::TFolder(TFile file) : path(file.Path()) {}

void TFolder::Create(mode_t mode) {
    int ret = mkdir(path.c_str(), mode);

    TLogger::LogAction("mkdir " + path, ret, errno);

    if (ret) {
        switch (errno) {
        case EEXIST:
            throw "Folder already exists";
        default:
            throw "Cannot create folder: " + string(strerror(errno));
        }
    }
}

void TFolder::Remove(bool recursive) {
    if (recursive) {
        for (auto f : Items(TFile::Any)) {
            TFile child(f);

            if (child.Type() == TFile::Directory)
                TFolder(f).Remove(recursive);
            else
                child.Remove();
        }
    }

    int ret = rmdir(path.c_str());

    TLogger::LogAction("rmdir " + path, ret, errno);

    if (ret)
        throw "Cannot remove folder: " + path;
}

void TFolder::Rename(std::string newname) {
    //TODO
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

vector<string> TFolder::Subfolders() {
    return Items(TFile::Directory);
}

vector<string> TFolder::Items(TFile::EFileType type) {
    DIR *dirp;
    struct dirent dp, *res;
    vector<string> ret;

    dirp = opendir(path.c_str());
    if (!dirp)
        throw "Cannot open folder " + path;
        
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
            ret.push_back(string(res->d_name));
    }

    closedir(dirp);
    return ret;
}
