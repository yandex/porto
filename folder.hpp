#ifndef __FOLDER_H__
#define __FOLDER_H__

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.hpp"

class TFolder {
    string path;

public:
    TFolder(string path) : path(path) {}

    void Create(mode_t mode) {
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

    void Remove() {
        int ret = rmdir(path.c_str());

        TLogger::LogAction("rmdir " + path, ret, errno);

        if (ret)
            throw "Cannot remove folder: " + path;
    }

    bool Exists() {
        struct stat st;

        int ret = stat(path.c_str(), &st);

        if (ret == 0 && S_ISDIR(st.st_mode))
            return true;
        else if (ret && errno == ENOENT)
            return false;
        else
            throw "Cannot stat foler: " + path;
    }
};

#endif
