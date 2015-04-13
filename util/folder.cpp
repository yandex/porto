#include <unordered_map>

#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/folder.hpp"
#include "util/mount.hpp"

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
        if (error)
            L_ERR() << "Can't remove " << Path << ": " << error << std::endl;
    }
}

TError TFolder::Create(mode_t mode, bool recursive) const {
    L() << "mkdir " << Path << std::endl;

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
        return TError(errno == ENOSPC ? EError::NoSpace : EError::Unknown, errno, "mkdir(" + Path.ToString() + ", " + std::to_string(mode) + ")");

    return TError::Success();
}

TError TFolder::Remove(bool recursive, bool silent) const {
    if (recursive) {
        vector<string> items;
        TError error = Items(EFileType::Any, items);
        if (error)
            return error;

        for (auto f : items) {
            TPath p(Path.AddComponent(f));
            TFile child(p);
            TError error;

            if (p.GetType() == EFileType::Directory)
                error = TFolder(p).Remove(recursive, true);
            else
                error = child.Remove(true);

            if (error)
                return error;
        }
    }

    if (!silent)
        L() << "rmdir " << Path << std::endl;

    if (!Path.Exists())
        return TError::Success();

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

TError TFolder::Copy(const TPath &dir) const {
    L() << "cp " << Path << " " << dir << std::endl;

    std::vector<std::string> list;
    TError error = Items(EFileType::Any, list);
    if (error)
        return error;

    for (auto &entry : list) {
        TPath from(Path.AddComponent(entry));
        TPath to(dir.AddComponent(entry));
        TFolder fromDir(from);
        TFolder toDir(to);
        switch(from.GetType()) {
        case EFileType::Directory:
            if (!toDir.Exists()) {
                error = toDir.Create(from.GetMode(), true);
                if (error)
                    return error;
            }

            error = fromDir.Copy(to);
            break;
        default:
            error = from.Copy(to);
            break;
        }

        if (error)
            return error;

        error = to.Chown(from.GetUid(), from.GetGid());
        if (error)
            return error;
    }

    return TError::Success();
}

void RemoveIf(const TPath &path,
              EFileType type,
              std::function<bool(const std::string &name, const TPath &path)> f) {
    std::vector<std::string> list;
    TFolder dir(path);

    TError error = dir.Items(type, list);
    if (error)
        return;

    for (auto &entry : list) {
        TPath id = path.AddComponent(entry);
        if (f(entry, id)) {
            L() << "Removing " << id << std::endl;
            TMount m(id, id, "", {});
            (void)m.Umount();

            if (id.GetType() == EFileType::Directory) {
                TFolder d(id);
                error = d.Remove(true);
            } else {
                TFile f(id);
                error = f.Remove();
            }
            if (error)
                L_WRN() << "Can't remove " << id << ": " << error << std::endl;
        }
    }
}
