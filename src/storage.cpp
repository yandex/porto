#include "storage.hpp"
#include "volume.hpp"
#include "helpers.hpp"
#include "config.hpp"
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

static TUintMap PlaceLoad;
static TUintMap PlaceLoadLimit;

void TStorage::Init() {
    if (StringToUintMap(config().volumes().place_load_limit(), PlaceLoadLimit))
        PlaceLoadLimit = {{"default", 1}};
}

void TStorage::IncPlaceLoad(const TPath &place) {
    auto lock = LockVolumes();
    auto id = place.ToString();
    if (!PlaceLoadLimit.count(id))
        id = "default";
    StorageCv.wait(lock, [&]{return PlaceLoad[id] < PlaceLoadLimit[id];});
    PlaceLoad[id]++;
}

void TStorage::DecPlaceLoad(const TPath &place) {
    auto lock = LockVolumes();
    auto id = place.ToString();
    if (!PlaceLoadLimit.count(id))
        id = "default";
    if (PlaceLoad[id]-- <= 1)
        PlaceLoad.erase(id);
    StorageCv.notify_all();
}

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

        TFile dirent;
        if (!dirent.OpenDir(path)) {
            if (PathIsActive(dirent.RealPath()))
                continue;

            path = dirent.RealPath();

        } else if (path.IsRegularStrict()) {
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
        L_ACT("Remove junk: {}", path);
        error = RemoveRecursive(path);
        if (error) {
            L_WRN("Cannot remove junk: {}: {}", path, error);

            error = path.RemoveAll();
            if (error)
                L_WRN("cannot delete junk: {}: {}", path, error);
        }
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
    if (name == "" || name == "." || name == ".."||
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
    struct stat st;
    TError error;
    TFile priv;

    error = CheckName(Name);
    if (error)
        return error;

    error = priv.Open(TempPath(PRIVATE_PREFIX),
                      O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
    if (error || priv.Stat(st)) {
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

static TError Compression(const TPath &archive, const TFile &arc,
                             const std::string &compress,
                             std::string &format, std::string &option) {
    std::string name = archive.BaseName();

    format = "tar";
    if (compress != "") {
        if (compress == "txz" || compress == "tar.xz")
            goto xz;
        if (compress == "tgz" || compress == "tar.gz")
            goto gz;
        if (compress == "tar")
            goto tar;
        if (StringEndsWith(compress, "squashfs"))
            goto squash;
        return TError(EError::InvalidValue, "Unknown archive " + archive.ToString() + " compression " + compress);
    }

    /* tar cannot guess compression for std streams */
    if (arc.Fd >= 0) {
        char magic[8];

        if (pread(arc.Fd, magic, sizeof(magic), 0) == sizeof(magic)) {
            if (!strncmp(magic, "\xFD" "7zXZ\x00", 6))
                goto xz;
            if (!strncmp(magic, "\x1F\x8B\x08", 3))
                goto gz;
            if (!strncmp(magic, "hsqs", 4))
                goto squash;
        }

        if (pread(arc.Fd, magic, sizeof(magic), 257) == sizeof(magic)) {
            /* "ustar\000" or "ustar  \0" */
            if (!strncmp(magic, "ustar", 5))
                goto tar;
        }

        return TError(EError::InvalidValue, "Cannot detect archive " + archive.ToString() + " compression by magic");
    }

    if (StringEndsWith(name, ".xz") || StringEndsWith(name, ".txz"))
        goto xz;

    if (StringEndsWith(name, ".gz") || StringEndsWith(name, ".tgz"))
        goto gz;

    if (StringEndsWith(name, ".squash") || StringEndsWith(name, ".squashfs"))
        goto squash;

tar:
    option = "--no-auto-compress";
    return TError::Success();
gz:
    option = "--gzip";
    return TError::Success();
xz:
    option = "--xz";
    return TError::Success();
squash:
    format = "squashfs";
    auto sep = compress.find('.');
    if (sep != std::string::npos)
        option = compress.substr(0, sep);
    else
        option = config().volumes().squashfs_compression();
    return TError::Success();
}

static bool TarSupportsXattrs() {
    static bool tested = false, result = false;
    if (!tested) {
        TFile null;
        result = !null.OpenReadWrite("/dev/null") &&
                 !RunCommand({ "tar", "--create", "--xattrs",
                               "--files-from", "/dev/null"}, "/", null, null);
        tested = true;
    }
    return result;
}

TError TStorage::ImportArchive(const TPath &archive, const std::string &compress, bool merge) {
    TPath temp = TempPath(IMPORT_PREFIX);
    TError error;
    TFile arc;

    error = CheckName(Name);
    if (error)
        return error;

    error = CL->CanControlPlace(Place);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    if (!archive.IsAbsolute())
        return TError(EError::InvalidValue, "archive path must be absolute");

    if (!archive.Exists())
        return TError(EError::InvalidValue, "archive not found");

    if (!archive.IsRegularFollow())
        return TError(EError::InvalidValue, "archive not a file");

    error = arc.OpenRead(archive);
    if (error)
        return error;

    error = CL->ReadAccess(arc);
    if (error)
        return TError(error, "Cannot import " + Name + " from " + archive.ToString());

    std::string compress_format, compress_option;
    error = Compression(archive, arc, compress, compress_format, compress_option);
    if (error)
        return error;

    auto lock = LockVolumes();

    TFile import_dir;

    while (!import_dir.OpenDir(temp) && PathIsActive(import_dir.RealPath())) {
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
            return TError(error, "Cannot merge " + Path.ToString());
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

    import_dir.OpenDir(temp);
    temp = import_dir.RealPath();

    ActivePaths.push_back(temp);
    lock.unlock();

    IncPlaceLoad(Place);

    if (compress_format == "tar") {
        TTuple args = { "tar",
                        "--numeric-owner",
                        "--preserve-permissions",
                        compress_option,
                        "--extract",
                        "-C", temp.ToString() };

        if (TarSupportsXattrs())
            args.insert(args.begin() + 3, {
                        "--xattrs",
                        "--xattrs-include=security.capability",
                        "--xattrs-include=trusted.overlay.*" });

        error = RunCommand(args, temp, arc, TFile());
    } else if (compress_format == "squashfs") {
        TTuple args = { "unsquashfs",
                        "-force",
                        "-no-progress",
                        "-processors", "1",
                        "-dest", temp.ToString(),
                        archive.ToString() };

        error = RunCommand(args, temp.DirName());
    } else
        error = TError(EError::NotSupported, "Unsuported format " + compress_format);

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

    DecPlaceLoad(Place);

    StorageCv.notify_all();

    return TError::Success();

err:
    TError error2 = temp.RemoveAll();
    if (error2)
        L_WRN("Cannot cleanup layer: {}", error2);

    DecPlaceLoad(Place);

    lock.lock();
    ActivePaths.remove(temp);
    lock.unlock();

    StorageCv.notify_all();

    return error;
}

TError TStorage::ExportArchive(const TPath &archive, const std::string &compress) {
    TFile dir, arc;
    TError error;

    error = CheckName(Name);
    if (error)
        return error;

    error = CL->CanControl(Owner);
    if (error)
        return TError(error, "Cannot export " + Path.ToString());

    if (!archive.IsAbsolute())
        return TError(EError::InvalidValue, "archive path must be absolute");

    if (archive.Exists())
        return TError(EError::InvalidValue, "archive already exists");

    std::string compress_format, compress_option;
    error = Compression(archive, TFile(), compress, compress_format, compress_option);
    if (error)
        return error;

    error = dir.OpenDir(archive.DirName());
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

    error = arc.OpenAt(dir, archive.BaseName(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0664);
    if (error)
        return error;

    IncPlaceLoad(Place);

    if (compress_format == "tar") {
        TTuple args = { "tar",
                        "--one-file-system",
                        "--numeric-owner",
                        "--preserve-permissions",
                        "--sparse",
                        "--transform", "s:^./::",
                        compress_option,
                        "--create",
                        "-C", Path.ToString(), "." };

        if (TarSupportsXattrs())
            args.insert(args.begin() + 4, "--xattrs");

        error = RunCommand(args, Path, TFile(), arc);
    } else if (compress_format == "squashfs") {
        TTuple args = { "mksquashfs", Path.ToString(),
                        archive.BaseName(),
                        "-noappend",
                        "-comp", compress_option };

        /* FIXME pass dir to fchdir */
        error = RunCommand(args, archive.DirName(), TFile());
    } else
        error = TError(EError::NotSupported, "Unsupported format " + compress_format);

    if (!error)
        error = arc.Chown(CL->TaskCred);
    if (error)
        (void)dir.UnlinkAt(archive.BaseName());

    DecPlaceLoad(Place);

    return error;
}

TError TStorage::Remove() {
    TPath temp;
    TError error;

    error = CL->CanControlPlace(Place);
    if (error && !StringStartsWith(Name, PORTO_WEAK_PREFIX))
        return error;

    error = CheckName(Name);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    error = Load();
    if (error)
        return error;

    error = CL->CanControl(Owner);
    if (error && !StringStartsWith(Name, PORTO_WEAK_PREFIX))
        return TError(error, "Cannot remove " + Path.ToString());

    auto lock = LockVolumes();

    error = CheckUsage();
    if (error)
        return error;

    temp = TempPath(PRIVATE_PREFIX);
    if (temp.Exists()) {
        error = temp.Unlink();
        if (error)
            L_WRN("Cannot remove private: {}", error);
    }

    TFile temp_dir;
    temp = TempPath(REMOVE_PREFIX + std::to_string(RemoveCounter++));

    error = Path.Rename(temp);
    if (!error) {
        error = temp_dir.OpenDir(temp);
        if (!error) {
            temp = temp_dir.RealPath();
            ActivePaths.push_back(temp);
        }
    }

    lock.unlock();

    if (error)
        return error;

    IncPlaceLoad(Place);

    error = RemoveRecursive(temp);
    if (error) {
        L_WRN("Cannot remove layer: {}", error);

        error = temp.RemoveAll();
        if (error)
            L_WRN("Cannot delete layer: {}", error);
    }

    DecPlaceLoad(Place);

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
