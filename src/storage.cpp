#include "storage.hpp"
#include "volume.hpp"
#include "helpers.hpp"
#include "filesystem.hpp"
#include "client.hpp"
#include <algorithm>
#include <condition_variable>
#include "util/unix.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

extern "C" {
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
}

static const char LAYER_TMP[] = "_tmp_";
static const char IMPORT_PREFIX[] = "_import_";
static const char REMOVE_PREFIX[] = "_remove_";
static const char PRIVATE_PREFIX[] = "_private_";

/* Protected with VolumesMutex */

static unsigned RemoveCounter = 0;

static std::list<TPath> ActivePaths;

static bool PathIsActive(const TPath &path) {
    PORTO_LOCKED(VolumesMutex);
    return std::find(ActivePaths.begin(), ActivePaths.end(), path) != ActivePaths.end();
}

static std::condition_variable StorageCv;

/* FIXME racy. rewrite with openat... etc */
TError TStorage::Cleanup(const TPath &place, const std::string &type, unsigned perms) {
    TPath base = place / type;
    struct stat st;
    TError error;

    error = base.StatStrict(st);
    if (error && error.GetErrno() == ENOENT) {
        /* In non-default place user must create base structure */
        if (place != PORTO_PLACE && (type == PORTO_VOLUMES || type == PORTO_LAYERS))
            return TError(EError::InvalidValue, base.ToString() + " must be directory");
        error = base.MkdirAll(perms);
        if (!error)
            error = base.StatStrict(st);
    }
    if (error)
        return error;

    if (!S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, base.ToString() + " must be directory");

    if (st.st_uid != RootUser || st.st_gid != PortoGroup) {
        error = base.Chown(RootUser, PortoGroup);
        if (error)
            return error;
    }

    if ((st.st_mode & 0777) != perms) {
        error = base.Chmod(perms);
        if (error)
            return error;
    }

    std::vector<std::string> list;
    error = base.ReadDirectory(list);
    if (error)
        return error;

    for (auto &name: list) {
        TPath path = base / name;

        if (path.IsDirectoryStrict() && !CheckName(name))
            continue;

        auto lock = LockVolumes();
        if (PathIsActive(path))
            continue;

        if (path.IsRegularStrict()) {
            if (type != PORTO_VOLUMES && StringStartsWith(name, PRIVATE_PREFIX)) {
                std::string tail = name.substr(std::string(PRIVATE_PREFIX).size());
                if ((base / tail).IsDirectoryStrict() ||
                        (base / (std::string(IMPORT_PREFIX) + tail)).IsDirectoryStrict())
                    continue;
            }

            /* Remove random files if any */
            path.Unlink();
            continue;
        }

        lock.unlock();
        L_ACT() << "Remove junk: " << path << std::endl;
        error = ClearRecursive(path);
        if (error)
            L_WRN() << "Cannot clear junk: " << path << ": " << error << std::endl;
        error = path.RemoveAll();
        if (error)
            L_WRN() << "cannot delete junk: " << path << " : " << error << std::endl;
    }

    return TError::Success();
}

TError TStorage::CheckPlace(const TPath &place) {
    TError error;

    if (!place.IsAbsolute() || !place.IsNormal())
        return TError(EError::InvalidValue, "place path must be normalized");

    if (IsSystemPath(place))
        return TError(EError::InvalidValue, "place in system directory");

    error = Cleanup(place, PORTO_VOLUMES, 0755);
    if (error)
        return error;

    error = Cleanup(place, PORTO_LAYERS, 0700);
    if (error)
        return error;

    error = Cleanup(place, PORTO_STORAGE, 0700);
    if (error)
        return error;

    return TError::Success();
}

TError TStorage::CheckName(const std::string &name) {
    auto pos = name.find_first_not_of(PORTO_NAME_CHARS);
    if (pos != std::string::npos)
        return TError(EError::InvalidValue, "forbidden character " +
                      StringFormat("%#x", (unsigned char)name[pos]));
    if (name == "." || name == ".."||
            StringStartsWith(name, LAYER_TMP) ||
            StringStartsWith(name, IMPORT_PREFIX) ||
            StringStartsWith(name, REMOVE_PREFIX) ||
            StringStartsWith(name, PRIVATE_PREFIX))
        return TError(EError::InvalidValue, "invalid layer name '" + name + "'");
    return TError::Success();
}

TError TStorage::List(const TPath &place, const std::string &type,
                      std::list<TStorage> &list) {
    std::vector<std::string> names;
    TError error = TPath(place / type).ListSubdirs(names);
    if (!error) {
        for (auto &name: names)
            if (!CheckName(name))
                list.emplace_back(place, type, name);
    }
    return error;
}

bool TStorage::Exists() {
    return Path.Exists();
}

uint64_t TStorage::LastUsage() {
    return LastChange ? (time(nullptr) - LastChange) : 0;
}

TError TStorage::CheckUsage() {
    PORTO_LOCKED(VolumesMutex);

    if (Type == PORTO_LAYERS) {
        if (!Exists())
            return TError(EError::LayerNotFound, "Layer " + Name + " not found");
        for (auto &it: Volumes) {
            for (auto &layer: it.second->Layers)
                if (Path == it.second->Place / PORTO_LAYERS / layer)
                    return TError(EError::Busy, "Layer " + Name + " in use by volume " + it.second->Path.ToString());
        }
    }

    if (Type == PORTO_STORAGE) {
        if (!Exists())
            return TError(EError::VolumeNotFound, "Storage " + Name + " not found");
        for (auto &it: Volumes) {
            if (Path == it.second->Place / PORTO_STORAGE / it.second->Storage)
                return TError(EError::Busy, "Storage " + Name + " in use by volume " + it.second->Path.ToString());
        }
    }

    return TError::Success();
}

TPath TStorage::TempPath(const std::string &kind) {
    return Place / Type / (kind + Name);
}

TError TStorage::Load() {
    TPath priv = TempPath(PRIVATE_PREFIX);
    struct stat st;
    TError error;

    error = priv.StatStrict(st);
    if (error) {
        if (error.GetErrno() != ENOENT)
            return error;
        error = Path.StatStrict(st);
        if (error) {
            if (error.GetErrno() == ENOENT) {
                if (Type == PORTO_LAYERS)
                    return TError(EError::LayerNotFound, "Layer " + Name + " not found");
                if (Type == PORTO_STORAGE)
                    return TError(EError::VolumeNotFound, "Storage " + Name + " not found");
            }
            return error;
        }
        Owner = TCred(NoUser, NoGroup);
        LastChange = st.st_mtime;
        Private = "";
        return TError::Success();
    }

    Owner = TCred(st.st_uid, st.st_gid);
    LastChange = st.st_mtime;
    error = priv.ReadAll(Private, 4096);
    if (error)
        Private = "";
    return error;
}

TError TStorage::SetOwner(const TCred &owner) {
    TPath priv = TempPath(PRIVATE_PREFIX);
    if (!priv.Exists())
        (void)priv.Mkfile(0644);
    TError error = priv.Chown(owner);
    if (!error)
        Owner = owner;
    return error;
}

TError TStorage::SetPrivate(const std::string &text) {
    TPath priv = TempPath(PRIVATE_PREFIX);
    if (!priv.Exists())
        (void)priv.Mkfile(0644);
    TError error = priv.WriteAll(text);
    if (!error)
        Private = text;
    return error;
}

TError TStorage::Touch() {
    TError error = TempPath(PRIVATE_PREFIX).Touch();
    if (error && error.GetErrno() == ENOENT)
        error = Path.Touch();
    return error;
}

static std::string TarCompression(const TPath &tarball, const TFile &file, const std::string &compress) {
    if (compress != "") {
        if (compress == "txz" || compress == "tar.xz")
            return "--xz";
        if (compress == "tgz" || compress == "tar.gz")
            return "--gzip";
        if (compress == "tar")
            return "--no-auto-compress";
        return "--no-auto-compress";
    }

    /* tar cannot guess compression for std streams */
    char magic[8];
    if (file.Fd >= 0 && pread(file.Fd, magic, sizeof(magic), 0) == sizeof(magic)) {
        if (!strncmp(magic, "\xFD" "7zXZ\x00", 6))
            return "--xz";
        if (!strncmp(magic, "\x1F\x8B\x08", 3))
            return "--gzip";
    }

    std::string name = tarball.BaseName();
    if (StringEndsWith(name, ".xz") || StringEndsWith(name, ".txz"))
        return "--xz";
    if (StringEndsWith(name, ".gz") || StringEndsWith(name, ".tgz"))
        return "--gzip";
    return "--no-auto-compress"; /* i.e. no compression */
}

TError TStorage::ImportTarball(const TPath &tarball, const std::string &compress, bool merge) {
    TPath temp = TempPath(IMPORT_PREFIX);
    TError error;
    TFile tar;

    error = CheckName(Name);
    if (error)
        return error;

    error = CL->CanControlPlace(Place);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    if (!tarball.Exists())
        return TError(EError::InvalidValue, "tarball not found");

    if (!tarball.IsRegularFollow())
        return TError(EError::InvalidValue, "tarball not a file");

    error = tar.OpenRead(tarball);
    if (error)
        return error;

    error = CL->ReadAccess(tar);
    if (error)
        return error;

    auto lock = LockVolumes();

    while (PathIsActive(temp)) {
        if (merge)
            return TError(EError::Busy, Name + " is importing right now");
        StorageCv.wait(lock);
    }

    if (merge && Exists()) {
        TStorage layer(Place, Type, Name);
        error = layer.Load();
        if (error)
            return error;
        error = CL->CanControl(layer.Owner);
        if (error)
            return error;
    }

    if (Path.Exists()) {
        if (!merge)
            return TError(EError::LayerAlreadyExists, "Layer already exists");
        error = CheckUsage();
        if (error)
            return error;
        error = Path.Rename(temp);
        if (error)
            return error;
    } else {
        /* first layer should not have whiteouts */
        merge = false;
        error = temp.Mkdir(0775);
        if (error)
            return error;
    }

    ActivePaths.push_back(temp);
    lock.unlock();

    error = RunCommand({ "tar",
                         "--numeric-owner",
                         "--preserve-permissions",
                         /* "--xattrs",
                            "--xattrs-include=security.capability",
                            "--xattrs-include=trusted.overlay.*", */
                         TarCompression(tarball, tar, compress),
                         "--extract",
                         "-C", temp.ToString() },
                         temp, tar, TFile());
    if (error)
        goto err;

    if (Type == PORTO_LAYERS) {
        error = SanitizeLayer(temp, merge);
        if (error)
            goto err;
    }

    if (!Owner.IsUnknown()) {
        error = SetOwner(Owner);
        if (error)
            goto err;
    }

    if (!Private.empty()) {
        error = SetPrivate(Private);
        if (error)
            goto err;
    }

    lock.lock();
    error = temp.Rename(Path);
    if (!error)
        ActivePaths.remove(temp);
    lock.unlock();
    if (error)
        goto err;

    StorageCv.notify_all();

    return TError::Success();

err:
    TError error2 = temp.RemoveAll();
    if (error2)
        L_WRN() << "Cannot cleanup layer: " << error2 << std::endl;

    lock.lock();
    ActivePaths.remove(temp);
    lock.unlock();

    StorageCv.notify_all();

    return error;
}

TError TStorage::ExportTarball(const TPath &tarball, const std::string &compress) {
    TFile dir, tar;
    TError error;

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    if (tarball.Exists())
        return TError(EError::InvalidValue, "tarball already exists");

    error = dir.OpenDir(tarball.DirName());
    if (error)
        return error;

    error = CL->WriteAccess(dir);
    if (error)
        return error;

    if (Type == PORTO_STORAGE) {
        auto lock = LockVolumes();
        error = CheckUsage();
        if (error)
            return error;
    }

    error = tar.OpenAt(dir, tarball.BaseName(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0664);
    if (error)
        return error;

    /*
     * FIXME tar in ubuntu precise knows nothing about xattrs,
     * maybe temporary convert opaque directories into aufs?
     */
    error = RunCommand({ "tar",
                        "--one-file-system",
                        "--numeric-owner",
                        "--preserve-permissions",
                        /* "--xattrs", */
                        "--sparse",
                        "--transform", "s:^./::",
                        TarCompression(tarball, TFile(), compress),
                        "--create",
                        "-C", Path.ToString(), "." },
                        Path, TFile(), tar);

    if (!error)
        error = tar.Chown(CL->TaskCred);
    if (error)
        (void)dir.UnlinkAt(tarball.BaseName());

    return error;
}

TError TStorage::Remove() {
    TPath temp;
    TError error;

    error = CheckName(Name);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    auto lock = LockVolumes();

    error = CheckUsage();
    if (error)
        return error;

    temp = TempPath(PRIVATE_PREFIX);
    if (temp.Exists()) {
        error = temp.Unlink();
        if (error)
            L_WRN() << "Cannot remove private: " << error << std::endl;
    }

    temp = TempPath(REMOVE_PREFIX + std::to_string(RemoveCounter++));
    error = Path.Rename(temp);
    if (!error)
        ActivePaths.push_back(temp);

    lock.unlock();

    if (error)
        return error;

    error = ClearRecursive(temp);
    if (error)
        L_WRN() << "Cannot clear layel: " << error << std::endl;

    error = temp.RemoveAll();
    if (error)
        L_WRN() << "Cannot remove layer: " << error << std::endl;

    lock.lock();
    ActivePaths.remove(temp);
    lock.unlock();

    return error;
}

/* FIXME recursive */
TError TStorage::SanitizeLayer(const TPath &layer, bool merge) {
    std::vector<std::string> content;

    TError error = layer.ReadDirectory(content);
    if (error)
        return error;

    for (auto &entry: content) {
        TPath path = layer / entry;

        /* Handle aufs whiteouts and metadata */
        if (entry.compare(0, 4, ".wh.") == 0) {

            /* Remove it completely */
            error = path.RemoveAll();
            if (error)
                return error;

            /* Opaque directory - hide entries in lower layers */
            if (entry == ".wh..wh..opq") {
                error = layer.SetXAttr("trusted.overlay.opaque", "y");
                if (error)
                    return error;
            }

            /* Metadata is done */
            if (entry.compare(0, 8, ".wh..wh.") == 0)
                continue;

            /* Remove whiteouted entry */
            path = layer / entry.substr(4);
            if (path.Exists()) {
                error = path.RemoveAll();
                if (error)
                    return error;
            }

            if (!merge) {
                /* Convert into overlayfs whiteout */
                error = path.Mknod(S_IFCHR, 0);
                if (error)
                    return error;
            }

            continue;
        }

        if (path.IsDirectoryStrict()) {
            error = SanitizeLayer(path, merge);
            if (error)
                return error;
        }
    }
    return TError::Success();
}
