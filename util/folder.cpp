#include <unordered_map>

#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/folder.hpp"

extern "C" {
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
}

using std::string;
using std::vector;
using std::unordered_map;

TFolder::~TFolder() {
    if (Tmp) {
        TError error = Remove(true);
        TLogger::LogError(error, "Can't remove " + Path.ToString());
    }
}

TError TFolder::Create(mode_t mode, bool recursive) const {
    TLogger::Log() << "mkdir " << Path.ToString() << std::endl;

    if (recursive) {
        string copy(Path.ToString());
        char *dup = strdup(copy.c_str());
        char *p = dirname(dup);
        TFolder f(p);
        free(dup);
        if (!f.Exists()) {
            TError error(f.Create(mode, true));
            if (error)
                return error;
        }
    }

    if (mkdir(Path.ToString().c_str(), mode) < 0)
        return TError(EError::Unknown, errno, "mkdir(" + Path.ToString() + ", " + std::to_string(mode) + ")");

    return TError::Success();
}

TError TFolder::Remove(bool recursive) const {
    if (recursive) {
        vector<string> items;
        TError error = Items(EFileType::Any, items);
        if (error)
            return error;

        for (auto f : items) {
            TPath p(Path);
            p.AddComponent(f);
            TFile child(p);
            TError error;

            if (p.GetType() == EFileType::Directory)
                error = TFolder(p).Remove(recursive);
            else
                error = child.Remove();

            if (error)
                return error;
        }
    }

    TLogger::Log() << "rmdir " << Path.ToString() << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return rmdir(Path.ToString().c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "rmdir(" + Path.ToString() + ")");

    return TError::Success();
}

TError TFolder::Subfolders(std::vector<std::string> &list) const {
    return Items(EFileType::Directory, list);
}

TError TFolder::Items(const EFileType type, std::vector<std::string> &list) const {
    DIR *dirp;
    struct dirent dp, *res;

    dirp = opendir(Path.ToString().c_str());
    if (!dirp)
        return TError(EError::Unknown, "Cannot open directory " + Path.ToString());

    while (!readdir_r(dirp, &dp, &res) && res != nullptr) {
        if (string(".") == res->d_name || string("..") == res->d_name)
            continue;

        static unordered_map<unsigned char, EFileType> d_type_to_type =
            {{DT_UNKNOWN, EFileType::Unknown},
             {DT_FIFO, EFileType::Fifo},
             {DT_CHR, EFileType::Character},
             {DT_DIR, EFileType::Directory},
             {DT_BLK, EFileType::Block},
             {DT_REG, EFileType::Regular},
             {DT_LNK, EFileType::Link},
             {DT_SOCK, EFileType::Socket}};

        if (type == EFileType::Any || type == d_type_to_type[res->d_type])
            list.push_back(string(res->d_name));
    }

    closedir(dirp);
    return TError::Success();
}
