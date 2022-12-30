#include <algorithm>
#include <condition_variable>

#include "storage.hpp"
#include "volume.hpp"
#include "helpers.hpp"
#include "config.hpp"
#include "filesystem.hpp"
#include "client.hpp"
#include "docker.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/md5.hpp"
#include "util/quota.hpp"
#include "util/thread.hpp"

extern "C" {
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
}

static const char LAYER_TMP[] = "_tmp_";
static const char WEAK_PREFIX[] = "_weak_";
static const char IMPORT_PREFIX[] = "_import_";
static const char REMOVE_PREFIX[] = "_remove_";
static const char ASYNC_REMOVE_PREFIX[] = "_asyncremove_";
static const char PRIVATE_PREFIX[] = "_private_";
static const char META_PREFIX[] = "_meta_";
static const char META_LAYER[] = "_layer_";

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

extern std::atomic_bool NeedStopHelpers;

static std::unique_ptr<std::thread> AsyncRemoveThread;
static std::condition_variable AsyncRemoverCv;
static std::mutex AsyncRemoverMutex;

static std::set<TPath> Places;
static std::mutex PlacesMutex;

static inline std::unique_lock<std::mutex> LockAsyncRemover() {
    return std::unique_lock<std::mutex>(AsyncRemoverMutex);
}

static inline std::unique_lock<std::mutex> LockPlaces() {
    return std::unique_lock<std::mutex>(PlacesMutex);
}


void AsyncRemoveWatchDog() {
    TError error;

    while (!NeedStopHelpers) {
        auto placesLock = LockPlaces();
        const auto places = Places;
        placesLock.unlock();

        for (const auto &place : places) {
            TPath base = place / PORTO_LAYERS;

            std::vector<std::string> list;

            auto asyncRemoverLock = LockAsyncRemover();
            error = base.ReadDirectory(list);
            asyncRemoverLock.unlock();

            if (error)
                continue;

            for (auto &name: list) {
                if (!StringStartsWith(name, ASYNC_REMOVE_PREFIX))
                    continue;

                TPath path = base / name;

                error = RemoveRecursive(path, true);
                if (error)
                    L_ERR("Can not async remove layer: {}", error);

                if (NeedStopHelpers)
                    return;
            }
        }

        auto asyncRemoverLock = LockAsyncRemover();
        AsyncRemoverCv.wait_for(asyncRemoverLock, std::chrono::milliseconds(5000));
    }
}

TError TStorage::Resolve(EStorageType type, const TPath &place, const std::string &name, bool strict) {
    TError error;

    Place = place;
    error = CL->ClientContainer->ResolvePlace(Place, strict);
    if (error)
        return error;

    Open(type, Place, name);
    return OK;
}

void TStorage::Open(EStorageType type, const TPath &place, const std::string &name) {
    PORTO_ASSERT(place.IsAbsolute());

    Type = type;
    Name = name;
    FirstName = name;
    Place = place;

    auto sep = name.find('/');
    if (sep != std::string::npos) {
        Meta = name.substr(0, sep);
        FirstName = name.substr(sep + 1);
    }

    switch (type) {
    case EStorageType::Place:
        Path = Place;
        break;
    case EStorageType::Layer:
        if (sep == std::string::npos)
            Path = Place / PORTO_LAYERS / Name;
        else
            Path = Place / PORTO_STORAGE / fmt::format("{}{}/{}{}", META_PREFIX, Meta, META_LAYER, name.substr(sep + 1));
        break;
    case EStorageType::DockerLayer:
        Path = TDockerImage::TLayer(Name).LayerPath(Place) / "content";
        break;
    case EStorageType::Storage:
        if (sep == std::string::npos)
            Path = Place / PORTO_STORAGE / Name;
        else
            Path = place / PORTO_STORAGE / fmt::format("{}{}/{}", META_PREFIX, Meta, name.substr(sep + 1));
        break;
    case EStorageType::Meta:
        Path = place / PORTO_STORAGE / fmt::format("{}{}", META_PREFIX, Name);
        break;
    case EStorageType::Volume:
        Path = place / PORTO_VOLUMES / name;
        break;
    }
}

void TStorage::Init() {
    if (StringToUintMap(config().volumes().place_load_limit(), PlaceLoadLimit))
        PlaceLoadLimit = {{"default", 1}};

    Places.insert("/place");
    AsyncRemoveThread = std::unique_ptr<std::thread>(NewThread(&AsyncRemoveWatchDog));
}

void TStorage::StopAsyncRemover() {
    AsyncRemoverCv.notify_all();
    AsyncRemoveThread->join();
}

void TStorage::IncPlaceLoad(const TPath &place) {
    auto lock = LockVolumes();
    auto id = place.ToString();

    if (!PlaceLoadLimit.count(id))
        id = "default";
    L_ACT("Start waiting for place load slot, id={} limit={}", id, PlaceLoadLimit[id]);
    StorageCv.wait(lock, [&]{return PlaceLoad[id] < PlaceLoadLimit[id];});
    PlaceLoad[id]++;
    L_ACT("Finish waiting for place load slot, id={}", id);
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
TError TStorage::Cleanup(const TPath &place, EStorageType type, unsigned perms) {
    TPath base;
    struct stat st;
    TError error;

    switch (type) {
    case EStorageType::Volume:
        base = place / PORTO_VOLUMES;
        break;
    case EStorageType::Layer:
        base = place / PORTO_LAYERS;
        break;
    case EStorageType::DockerLayer:
        base = place / PORTO_DOCKER_LAYERS;
        break;
    case EStorageType::Storage:
        base = place / PORTO_STORAGE;
        break;
    case EStorageType::Meta:
        base = place;
        break;
    case EStorageType::Place:
        break;
    }

    error = base.StatStrict(st);
    if (error && error.Errno == ENOENT) {
        /* In non-default place user must create base structure */
        bool default_place = false;
        if (place == PORTO_PLACE)
            default_place = true;
        else {
            for (const auto &path : AuxPlacesPaths) {
                if (place == path)
                    default_place = true;
            }
        }

        if (!default_place && (type == EStorageType::Volume || type == EStorageType::Layer))
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
        if (StringStartsWith(name, ASYNC_REMOVE_PREFIX))
            continue;

        TPath path = base / name;

        if (type == EStorageType::Storage && path.IsDirectoryStrict() &&
                StringStartsWith(name, META_PREFIX)) {
            error = Cleanup(path, EStorageType::Meta, 0700);
            if (error)
                L_WRN("Cannot cleaup metastorage {} {}", path, error);
            continue;
        }

        if (path.IsDirectoryStrict()) {
            if (!CheckName(name))
                continue;
            if (type == EStorageType::Meta && StringStartsWith(name, META_LAYER))
                continue;
        }

        auto lock = LockVolumes();

        TFile dirent;
        if (!dirent.OpenDir(path)) {
            if (PathIsActive(dirent.RealPath()))
                continue;

            path = dirent.RealPath();

        } else if (path.IsRegularStrict()) {
            if (type != EStorageType::Volume && StringStartsWith(name, PRIVATE_PREFIX)) {
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
            L_VERBOSE("Cannot remove junk {}: {}", path, error);
            error = path.RemoveAll();
            if (error)
                L_WRN("cannot remove junk {}: {}", path, error);
        }
    }

    return OK;
}

TError TStorage::CheckPlace(const TPath &place) {
    TError error;

    if (!place.IsAbsolute())
        return TError(EError::InvalidPath, "Place path {} must be absolute", place);

    if (!place.IsNormal())
        return TError(EError::InvalidPath, "Place path {} must be normalized", place);

    if (IsSystemPath(place))
        return TError(EError::InvalidPath, "Place path {} in system directory", place);

    error = Cleanup(place, EStorageType::Volume, 0755);
    if (error)
        return error;

    error = Cleanup(place, EStorageType::Layer, 0700);
    if (error)
        return error;

    error = Cleanup(place, EStorageType::Storage, 0700);
    if (error)
        return error;

    if (config().daemon().docker_images_support()) {
        error = Cleanup(place, EStorageType::DockerLayer, 0700);
        if (error)
            return error;

        error = TDockerImage::InitStorage(place, 0700);
        if (error)
            return error;
    }

    auto lockPlaces = LockPlaces();
    Places.insert(place);
    lockPlaces.unlock();

    return OK;
}

TError TStorage::CheckName(const std::string &name, bool meta) {
    auto sep = name.find('/');
    if (!meta && sep != std::string::npos) {
        TError error = CheckName(name.substr(0, sep), true);
        if (error)
            return error;
        return CheckName(name.substr(sep + 1), true);
    }
    auto pos = name.find_first_not_of(PORTO_NAME_CHARS);
    if (pos != std::string::npos)
        return TError(EError::InvalidValue, "forbidden character " +
                      StringFormat("%#x", (unsigned char)name[pos]));
    if (name == "" || name == "." || name == ".."||
            StringStartsWith(name, LAYER_TMP) ||
            StringStartsWith(name, IMPORT_PREFIX) ||
            StringStartsWith(name, REMOVE_PREFIX) ||
            StringStartsWith(name, ASYNC_REMOVE_PREFIX) ||
            StringStartsWith(name, PRIVATE_PREFIX) ||
            StringStartsWith(name, META_PREFIX) ||
            StringStartsWith(name, META_LAYER))
        return TError(EError::InvalidValue, "invalid layer name '" + name + "'");
    return OK;
}

TError TStorage::List(EStorageType type, std::list<TStorage> &list) {
    std::vector<std::string> names;
    TPath path = Path;

    if (Type == EStorageType::Place) {
        if (type == EStorageType::Layer)
            path = Place / PORTO_LAYERS;
        else if (type == EStorageType::DockerLayer) {
            path = Place / PORTO_DOCKER_LAYERS / "blobs";
            if (!path.Exists())
                return OK;
        } else
            path = Place / PORTO_STORAGE;
    }

    TError error = path.ListSubdirs(names);
    if (error)
        return error;

    if (type == EStorageType::DockerLayer) {
        std::vector<std::string> prefixes = std::move(names);
        names.clear();
        for (auto &prefix: prefixes) {
            std::vector<std::string> prefixNames;
            error = TPath(path / prefix).ListSubdirs(prefixNames);
            if (error) {
                L_WRN("Cannot list subdirs {}: {}", path / prefix, error);
                continue;
            }
            names.insert(names.end(), prefixNames.begin(), prefixNames.end());
        }
    }

    for (auto &name: names) {
        if (Type == EStorageType::Place && StringStartsWith(name, META_PREFIX)) {
            TStorage meta;
            meta.Open(EStorageType::Meta, Place, name.substr(std::string(META_PREFIX).size()));
            list.push_back(meta);
            if (type == EStorageType::Storage) {
                error = meta.List(type, list);
                if (error)
                    return error;
            }
        } else if (Type == EStorageType::Meta) {
            if (StringStartsWith(name, META_LAYER)) {
                if (type == EStorageType::Layer) {
                    TStorage layer;
                    layer.Open(EStorageType::Layer, Place, Name + "/" + name.substr(std::string(META_LAYER).size()));
                    list.push_back(layer);
                }
            } else if (type == EStorageType::Storage && !CheckName(name)) {
                TStorage storage;
                storage.Open(EStorageType::Storage, Place, Name + "/" + name);
                list.push_back(storage);
            }
        } else if (!CheckName(name)) {
            TStorage storage;
            storage.Open(type, Place, name);
            list.push_back(storage);
        }
    }

    if (Type == EStorageType::Place && type == EStorageType::Layer) {
        names.clear();
        error = TPath(Place / PORTO_STORAGE).ListSubdirs(names);
        if (error) {
            if (!TPath(Place / PORTO_STORAGE).Exists())
                return OK;
            return error;
        }
        for (auto &name: names) {
            if (StringStartsWith(name, META_PREFIX)) {
                TStorage meta;
                meta.Open(EStorageType::Meta, Place, name.substr(std::string(META_PREFIX).size()));
                error = meta.List(EStorageType::Layer, list);
                if (error)
                    return error;
            }
        }
    }

    return error;
}

bool TStorage::Exists() const {
    return Path.Exists();
}

bool TStorage::Weak() const {
    return StringStartsWith(FirstName, WEAK_PREFIX);
}

uint64_t TStorage::LastUsage() const {
    return LastChange ? (time(nullptr) - LastChange) : 0;
}

TError TStorage::CheckUsage() {
    PORTO_LOCKED(VolumesMutex);

    if (Type == EStorageType::Layer) {
        if (!Exists())
            return TError(EError::LayerNotFound, "Layer " + Name + " not found");
        for (auto &it: Volumes) {
            for (auto &layer: it.second->Layers)
                if (Place == it.second->Place && Name == layer)
                    return TError(EError::Busy, "Layer " + Name + " in use by volume " + it.second->Path.ToString());
        }
    }

    if (Type == EStorageType::Storage) {
        if (!Exists())
            return TError(EError::VolumeNotFound, "Storage " + Name + " not found");
        for (auto &it: Volumes) {
            if (Place == it.second->Place && Name == it.second->Storage)
                return TError(EError::Busy, "Storage " + Name + " in use by volume " + it.second->Path.ToString());
        }
    }

    if (Type == EStorageType::Meta) {
        struct stat st;
        TError error = Path.StatStrict(st);
        if (error)
            return error;
        if (st.st_nlink != 2)
            return TError(EError::Busy, "MetaStorage {} in use, {} not empty", Name, Path);
    }

    return OK;
}

TPath TStorage::TempPath(const std::string &kind) {
    return Path.DirNameNormal() / kind + Path.BaseNameNormal();
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
        if (error.Errno != ENOENT)
            return error;
        error = Path.StatStrict(st);
        if (error) {
            if (error.Errno == ENOENT) {
                if (Type == EStorageType::Layer)
                    return TError(EError::LayerNotFound, "Layer " + Name + " not found");
                if (Type == EStorageType::Storage)
                    return TError(EError::VolumeNotFound, "Storage " + Name + " not found");
            }
            return error;
        }
        Owner = TCred(NoUser, NoGroup);
        LastChange = st.st_mtime;
        Private = "";
        return OK;
    }

    Owner = TCred(st.st_uid, st.st_gid);
    LastChange = st.st_mtime;
    error = priv.ReadAll(Private, 4096);
    if (error)
        Private = "";
    return error;
}

TError TStorage::SaveOwner(const TCred &owner) {
    TPath priv = TempPath(PRIVATE_PREFIX);
    if (!priv.Exists())
        (void)priv.Mkfile(0644);
    TError error = priv.Chown(owner);
    if (!error)
        Owner = owner;
    return error;
}

TError TStorage::SetPrivate(const std::string &text) {
    TError error;

    if (text.size() > PRIVATE_VALUE_MAX)
        return TError(EError::InvalidValue, "Private value too log, max {} bytes", PRIVATE_VALUE_MAX);

    error = Load();
    if (error)
        return error;
    error = CL->CanControl(Owner);
    if (error)
        return TError(error, "Cannot set private {}", Name);
    return SavePrivate(text);
}

TError TStorage::SavePrivate(const std::string &text) {
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
    if (error && error.Errno == ENOENT)
        error = Path.Touch();
    return error;
}

static bool TarSupportsCompressArgs() {
    static bool tested = false, result = false;
    if (!tested) {
        TFile null;
        result = !null.OpenReadWrite("/dev/null") &&
                 !RunCommand({ "tar", "--create", "--use-compress-program=gzip --best",
                               "--files-from", "/dev/null"}, TFile(), null, null);
        L_SYS("tar {}supports compress program arguments", result ? "" : "not ");
        tested = true;
    }
    return result;
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
        if (compress == "tzst" || compress == "tar.zst")
            goto zst;
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
            if (!strncmp(magic, "\x28\xB5\x2F\xFD", 4))
                goto zst;
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

    if (StringEndsWith(name, ".zst") || StringEndsWith(name, ".tzst"))
        goto zst;

    if (StringEndsWith(name, ".squash") || StringEndsWith(name, ".squashfs"))
        goto squash;

tar:
    option = "--no-auto-compress";
    return OK;
gz:
    if (!arc && config().volumes().parallel_compression()) {
        if (TPath("/usr/bin/pigz").Exists()) {
            option = "--use-compress-program=pigz";
            return OK;
        }
    }
    option = "--gzip";
    return OK;
xz:
    if (!arc && config().volumes().parallel_compression()) {
        if (TPath("/usr/bin/pixz").Exists()) {
            option = "--use-compress-program=pixz";
            return OK;
        }
    }
    option = "--xz";
    return OK;
zst:
    if (!arc && config().volumes().parallel_compression()) {
        if (TPath("/usr/bin/zstdmt").Exists()) {
            if (TarSupportsCompressArgs())
                option = "--use-compress-program=zstdmt -19";
            else
                option = "--use-compress-program=zstdmt ";
            return OK;
        }
    }
    if (TPath("/usr/bin/zstd").Exists()) {
        if (!arc && TarSupportsCompressArgs())
            option = "--use-compress-program=zstd -19";
        else
            option = "--use-compress-program=zstd";
        return OK;
    }
    return TError(EError::NotSupported, "Compression: Can not find /usr/bin/zstd binary" );
squash:
    format = "squashfs";
    auto sep = compress.find('.');
    if (sep != std::string::npos)
        option = compress.substr(0, sep);
    else
        option = config().volumes().squashfs_compression();
    return OK;
}

static bool TarSupportsXattrs() {
    static bool tested = false, result = false;
    if (!tested) {
        TFile null;
        result = !null.OpenReadWrite("/dev/null") &&
                 !RunCommand({ config().daemon().tar_path(), "--create", "--xattrs",
                               "--files-from", "/dev/null"}, TFile(), null, null);
        L_SYS("tar {}supports extended attributes", result ? "" : "not ");
        tested = true;
    }
    return result;
}

TError TStorage::SaveChecksums() {
    TPathWalk walk;
    TError error;

    error = walk.OpenScan(Path);
    if (error)
        return error;

    Size = 0;

    while (1) {
        error = walk.Next();
        if (error)
            return error;
        if (!walk.Path)
            break;
        if (!walk.Postorder)
            Size += walk.Stat->st_blocks * 512ull;
        if (!S_ISREG(walk.Stat->st_mode))
            continue;
        TFile file;
        error = file.OpenRead(walk.Path);
        if (error)
            return error;
        std::string sum;
        error = Md5Sum(file, sum);
        if (error)
            return error;
        error = file.SetXAttr("user.porto.md5sum", sum);
        if (error)
            return error;
    }

    return OK;
}

TError TStorage::ImportArchive(const TPath &archive, const std::string &memCgroup, const std::string &compress, bool merge, bool verboseError) {
    TPath temp = TempPath(IMPORT_PREFIX);
    TError error;
    TFile arc;

    error = CheckName(Name);
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
        return TError(error, "Cannot import {} from {}", Name, archive);

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
        TStorage layer;
        layer.Open(Type, Place, Name);
        error = layer.Load();
        if (error)
            return error;
        error = CL->CanControl(layer.Owner);
        if (error)
            return TError(error, "Cannot merge {}", Path);
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
        error = temp.Mkdir(0775);
        if (error)
            return error;
    }

    import_dir.OpenDir(temp);
    temp = import_dir.RealPath();

    ActivePaths.push_back(temp);
    lock.unlock();

    IncPlaceLoad(Place);
    Statistics->LayerImport++;

    if (compress_format == "tar") {
        TTuple args = { config().daemon().tar_path(),
                        "--numeric-owner",
                        "--preserve-permissions",
                        compress_option,
                        "--extract" };

        if (TarSupportsXattrs())
            args.insert(args.begin() + 3, {
                        "--xattrs",
                        "--xattrs-include=security.capability",
                        "--xattrs-include=trusted.overlay.*",
                        "--xattrs-include=user.*"});

        error = RunCommand(args, {}, import_dir, arc, TFile(), HelperCapabilities, memCgroup, verboseError, true);
    } else if (compress_format == "squashfs") {
        TTuple args = { "unsquashfs",
                        "-force",
                        "-no-progress",
                        "-processors", "1",
                        "-dest", temp.ToString(),
                        archive.ToString() };

        TFile parent_dir;
        error = parent_dir.OpenDirStrictAt(import_dir, "..");
        if (error)
            return error;

        error = RunCommand(args, {}, parent_dir, TFile(), TFile(), HelperCapabilities, memCgroup, verboseError, true);
    } else
        error = TError(EError::NotSupported, "Unsuported format " + compress_format);

    if (error)
        goto err;

    if (!merge && Type == EStorageType::Layer) {
        error = SanitizeLayer(temp);
        if (error)
            goto err;
    }

    if (!Owner.IsUnknown()) {
        error = SaveOwner(Owner);
        if (error)
            goto err;
    }

    if (!Private.empty()) {
        error = SavePrivate(Private);
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

    return OK;

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
        return TError(error, "Cannot export {}", Path);

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

    if (Type == EStorageType::Storage) {
        auto lock = LockVolumes();
        error = CheckUsage();
        if (error)
            return error;
    }

    error = arc.OpenAt(dir, archive.BaseName(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0664);
    if (error)
        return error;

    IncPlaceLoad(Place);
    Statistics->LayerExport++;

    if (Type == EStorageType::Volume && compress_format == "tar") {
        L_ACT("Save checksums in {}", Path);
        error = SaveChecksums();
        if (error)
            return error;
        L("Unpacked size {} {}", Path, StringFormatSize(Size));
        error = arc.SetXAttr("user.porto.unpacked_size", std::to_string(Size));
        if (error)
            L_WRN("Cannot save unpacked size in xattr: {}", error);
    }

    if (compress_format == "tar") {
        TTuple args = { config().daemon().tar_path(),
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

        error = RunCommand(args, dir, TFile(), arc);
    } else if (compress_format == "squashfs") {
        TTuple args = { "mksquashfs", Path.ToString(),
                        archive.BaseName(),
                        "-noappend",
                        "-comp", compress_option };

        error = RunCommand(args, dir, TFile());
    } else
        error = TError(EError::NotSupported, "Unsupported format " + compress_format);

    if (!error)
        error = arc.Chown(CL->TaskCred);
    if (error)
        (void)dir.UnlinkAt(archive.BaseName());

    DecPlaceLoad(Place);

    return error;
}

TError TStorage::Remove(bool weak, bool async) {
    TPath temp;
    TError error;

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
    if (error && !weak)
        return TError(error, "Cannot remove {}", Path);

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
    temp = TempPath((async ? ASYNC_REMOVE_PREFIX : REMOVE_PREFIX) + std::to_string(RemoveCounter++));

    std::unique_lock<std::mutex> asyncRemoverLock;
    if (async)
        asyncRemoverLock = LockAsyncRemover();

    error = Path.Rename(temp);
    if (!error) {
        error = temp_dir.OpenDir(temp);
        if (!error) {
            temp = temp_dir.RealPath();
            ActivePaths.push_back(temp);
        }
    }

    if (async)
        asyncRemoverLock.unlock();
    lock.unlock();

    if (error)
        return error;

    IncPlaceLoad(Place);
    Statistics->LayerRemove++;

    if (Type == EStorageType::Meta) {
        TProjectQuota quota(temp);
        error = quota.Destroy();
        if (error)
            L_WRN("Cannot destroy quota {}: {}", temp, error);
    }

    if (!async) {
        error = RemoveRecursive(temp);
            if (error) {
                L_VERBOSE("Cannot remove storage {}: {}", temp, error);
                error = temp.RemoveAll();
                if (error)
                    L_WRN("Cannot remove storage {}: {}", temp, error);
            }
    }

    DecPlaceLoad(Place);

    lock.lock();
    ActivePaths.remove(temp);
    lock.unlock();

    return error;
}

TError TStorage::SanitizeLayer(const TPath &layer) {
    TPathWalk walk;
    TError error;

    error = walk.Open(layer);
    if (error)
        return error;

    while (true) {
        error = walk.Next();
        if (error || !walk.Path)
            return error;

        /* Handle aufs whiteouts and metadata */
        if (StringStartsWith(walk.Name(), ".wh.")) {

            /* Remove it completely */
            error = walk.Path.RemoveAll();
            if (error)
                return error;

            /* Opaque directory - hide entries in lower layers */
            if (walk.Name() == ".wh..wh..opq") {
                error = walk.Path.DirName().SetXAttr("trusted.overlay.opaque", "y");
                if (error)
                    return error;
            }

            /* Metadata is done */
            if (StringStartsWith(walk.Name(), ".wh..wh."))
                continue;

            /* Remove whiteouted entry */
            TPath real = walk.Path.DirName() / walk.Name().substr(4);
            if (real.PathExists()) {
                error = real.RemoveAll();
                if (error)
                    return error;
            }

            /* Convert into overlayfs whiteout */
            error = real.Mknod(S_IFCHR, 0);
            if (error)
                return error;
        }
    }
}

TError TStorage::CreateMeta(uint64_t space_limit, uint64_t inode_limit) {
    TError error;

    error = CheckName(Name, true);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    auto lock = LockVolumes();

    error = Path.Mkdir(0700);
    if (error)
        return error;

    lock.unlock();

    TProjectQuota quota(Path);
    quota.SpaceLimit = space_limit;
    quota.InodeLimit = inode_limit;

    error = quota.Create();
    if (error)
        goto err;

    if (!Owner.IsUnknown()) {
        error = SaveOwner(Owner);
        if (error)
            goto err;
    }

    if (!Private.empty()) {
        error = SavePrivate(Private);
        if (error)
            goto err;
    }

    return OK;

err:
    Path.Rmdir();
    return error;
}

TError TStorage::ResizeMeta(uint64_t space_limit, uint64_t inode_limit) {
    TError error;

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
    if (error)
        return TError(error, "Cannot resize {}", Path);

    auto lock = LockVolumes();
    TProjectQuota quota(Path);
    quota.SpaceLimit = space_limit;
    quota.InodeLimit = inode_limit;
    return quota.Resize();
}
