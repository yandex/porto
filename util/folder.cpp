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
#include <pwd.h>
#include <grp.h>
}

using std::string;
using std::vector;
using std::unordered_map;

TFolder::~TFolder() {
    if (Tmp) {
        TError error = Remove(true);
        TLogger::LogError(error, "Can't remove " + Path);
    }
}

TError TFolder::Create(mode_t mode, bool recursive) const {
    TLogger::Log() << "mkdir " << Path << std::endl;

    if (recursive) {
        string copy(Path);
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

    if (mkdir(Path.c_str(), mode) < 0)
        return TError(EError::Unknown, errno, "mkdir(" + Path + ", " + std::to_string(mode) + ")");

    return TError::Success();
}

TError TFolder::Remove(bool recursive) const {
    if (recursive) {
        vector<string> items;
        TError error = Items(TFile::Any, items);
        if (error)
            return error;

        for (auto f : items) {
            string path = Path + "/" + f;
            TFile child(path);
            TError error;

            if (child.Type() == TFile::Directory)
                error = TFolder(path).Remove(recursive);
            else
                error = child.Remove();

            if (error)
                return error;
        }
    }

    TLogger::Log() << "rmdir " << Path << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return rmdir(Path.c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "rmdir(" + Path + ")");

    return TError::Success();
}

bool TFolder::Exists() const {
    struct stat st;

    int ret = stat(Path.c_str(), &st);

    if (ret == 0 && S_ISDIR(st.st_mode))
        return true;
    else
        return false;
}

TError TFolder::Subfolders(std::vector<std::string> &list) const {
    return Items(TFile::Directory, list);
}

TError TFolder::Items(const TFile::EFileType type, std::vector<std::string> &list) const {
    DIR *dirp;
    struct dirent dp, *res;

    dirp = opendir(Path.c_str());
    if (!dirp)
        return TError(EError::Unknown, "Cannot open directory " + Path);

    while (!readdir_r(dirp, &dp, &res) && res != nullptr) {
        if (string(".") == res->d_name || string("..") == res->d_name)
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
    return TError::Success();
}

TError TFolder::Chown(const std::string &user, const std::string &group) const {
    int uid, gid;

    struct passwd *p = getpwnam(user.c_str());
    if (!p)
        return TError(EError::InvalidValue, EINVAL, "getpwnam(" + user + ")");

    uid = p->pw_uid;

    struct group *g = getgrnam(group.c_str());
    if (!g)
        return TError(EError::InvalidValue, EINVAL, "getgrnam(" + group + ")");

    gid = g->gr_gid;

    int ret = chown(Path.c_str(), uid, gid);
    if (ret)
        return TError(EError::Unknown, errno, "chown(" + Path + ", " + user + ", " + group + ")");

    return TError::Success();
}
