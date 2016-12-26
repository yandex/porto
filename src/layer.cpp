#include <algorithm>
#include <layer.hpp>
#include <volume.hpp>
#include <config.hpp>
#include "helpers.hpp"
#include <condition_variable>
#include <util/unix.hpp>
#include <util/log.hpp>
#include <util/string.hpp>

extern "C" {
#include <sys/stat.h>
}

static unsigned LayerRemoveCounter = 0;

bool LayerIsJunk(const std::string &name) {
    return StringStartsWith(name, LAYER_TMP_PREFIX) ||
           StringStartsWith(name, LAYER_IMPORT_PREFIX) ||
           StringStartsWith(name, LAYER_REMOVE_PREFIX) ||
           StringStartsWith(name, LAYER_PRIVATE_PREFIX);
}

static std::list<TPath> ActivePaths;
static bool PathIsActive(const TPath &path) {
    return std::find(ActivePaths.begin(), ActivePaths.end(), path) != ActivePaths.end();
}

static std::condition_variable LayersCv;

TError CheckPlace(const TPath &place, bool init) {
    struct stat st;
    TError error;

    if (!place.IsAbsolute() || !place.IsNormal())
        return TError(EError::InvalidValue, "place path must be normalized");

    TPath volumes = place / PORTO_VOLUMES;
    if (init && !volumes.IsDirectoryStrict()) {
        (void)volumes.Unlink();
        error = volumes.MkdirAll(0755);
        if (error)
            return error;
    }
    error = volumes.StatStrict(st);
    if (error || !S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, "in place " + volumes.ToString() + " must be directory");
    if (st.st_uid != RootUser || st.st_gid != PortoGroup)
        volumes.Chown(RootUser, PortoGroup);
    if ((st.st_mode & 0777) != 0755)
        volumes.Chmod(0755);

    TPath layers = place / PORTO_LAYERS;
    if (init && !layers.IsDirectoryStrict()) {
        (void)layers.Unlink();
        error = layers.MkdirAll(0700);
        if (error)
            return error;
    }
    error = layers.StatStrict(st);
    if (error || !S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, "in place " + layers.ToString() + " must be directory");
    if (st.st_uid != RootUser || st.st_gid != PortoGroup)
        layers.Chown(RootUser, PortoGroup);
    if ((st.st_mode & 0777) != 0700)
        layers.Chmod(0700);

    std::vector<std::string> list;
    error = layers.ReadDirectory(list);
    if (error)
        return error;

    for (auto &layer: list) {
        TPath path = layers / layer;

        if (path.IsDirectoryStrict() && !LayerIsJunk(layer))
            continue;

        auto lock = LockVolumes();
        if (PathIsActive(path))
            continue;

        if (path.IsRegularStrict()) {
            if (StringStartsWith(layer, LAYER_PRIVATE_PREFIX)) {
                std::string name = layer.substr(std::string(LAYER_PRIVATE_PREFIX).size());
                std::string importing = LAYER_IMPORT_PREFIX + name;

                if ((layers / name).Exists() || (layers / importing).Exists())
                    continue;
            }

            /* Cleanup random files if any */
            path.Unlink();

            continue;
        }

        lock.unlock();

        error = ClearRecursive(path);
        if (error)
            L_WRN() << "Cannot clear junk layer: " << path << ": " << error << std::endl;

        error = path.RemoveAll();
        if (error)
            L_WRN() << "cannot delete junk layer: " << path << " : " << error << std::endl;
    }

    TPath storage = place / PORTO_STORAGE;
    if (init && !storage.IsDirectoryStrict()) {
        (void)storage.Unlink();
        error = storage.MkdirAll(0700);
        if (error)
            return error;
    }

    error = storage.StatStrict(st);
    if (error || !S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, "in storage " + storage.ToString() + " must be directory");
    if (st.st_uid != RootUser || st.st_gid != PortoGroup)
        storage.Chown(RootUser, PortoGroup);
    if ((st.st_mode & 0777) != 0700)
        storage.Chmod(0700);

    return TError::Success();
}

TError ValidateLayerName(const std::string &name) {
    auto pos = name.find_first_not_of(PORTO_NAME_CHARS);
    if (pos != std::string::npos)
        return TError(EError::InvalidValue,
                "forbidden character '" + name.substr(pos, 1) + "' in layer name");
    if (name == "." || name == ".."|| LayerIsJunk(name))
        return TError(EError::InvalidValue, "invalid layer name '" + name + "'");
    return TError::Success();
}

bool LayerInUse(const std::string &name, const TPath &place) {
    for (auto &volume : Volumes) {
        if (volume.second->Place != place)
            continue;
        auto &layers = volume.second->Layers;
        if (std::find(layers.begin(), layers.end(), name) != layers.end())
            return true;
    }
    return false;
}

TError LayerOwner(const std::string &name, const TPath &place, TCred &owner) {
    TPath private_file = place / PORTO_LAYERS / LAYER_PRIVATE_PREFIX + name;

    if (private_file.Exists()) {
        struct stat st;
        TError error = private_file.StatStrict(st);
        if (error)
            return error;
        owner = TCred(st.st_uid, st.st_gid);
    } else
        owner = TCred(NoUser, NoGroup);

    return TError::Success();
}

uint64_t LayerLastUsage(const std::string &name, const TPath &place) {
    struct stat st;
    if (!(place / PORTO_LAYERS / LAYER_PRIVATE_PREFIX + name).StatStrict(st))
        return time(NULL) - st.st_mtime;
    else if (!(place / PORTO_LAYERS / name).StatStrict(st))
        return time(NULL) - st.st_mtime;
    return 0;
}

TError ImportLayer(const std::string &name, const TPath &place,
                   const TPath &tarball, bool merge,
                   const std::string private_value,
                   const TCred &owner) {
    TPath layers = place / PORTO_LAYERS;
    TPath layer = layers / name;
    TPath layer_tmp = layers / LAYER_IMPORT_PREFIX + name;
    TPath layer_private = layers / LAYER_PRIVATE_PREFIX + name;
    TFile private_file;
    TError error;

    error = ValidateLayerName(name);
    if (error)
        return error;

    auto volumes_lock = LockVolumes();

    while (PathIsActive(layer_tmp)) {
        if (merge)
            return TError(EError::Busy, "the layer is busy");
        LayersCv.wait(volumes_lock);
    }

    if (layer.Exists()) {
        if (!merge)
            return TError(EError::LayerAlreadyExists, "Layer already exists");
        if (LayerInUse(name, place))
            return TError(EError::Busy, "layer in use");
        error = layer.Rename(layer_tmp);
        if (error)
            return error;
    } else {
        /* first layer should not have whiteouts */
        if (merge)
            merge = false;

        error = layer_tmp.Mkdir(0755);
        if (error)
            return error;
    }

    ActivePaths.push_back(layer_tmp);
    volumes_lock.unlock();

    error = UnpackTarball(tarball, layer_tmp);
    if (error)
        goto err;

    error = SanitizeLayer(layer_tmp, merge);
    if (error)
        goto err;

    if (!merge || !layer_private.Exists())
        error = private_file.CreateNew(layer_private, 0600);
    else
        error = private_file.OpenTrunc(layer_private);

    if (error)
        goto err;

    error = private_file.WriteAll(private_value);
    if (error)
        goto err;

    error = private_file.Chown(owner);
    if (error)
        goto err;

    volumes_lock.lock();
    error = layer_tmp.Rename(layer);
    if (!error)
        ActivePaths.remove(layer_tmp);
    volumes_lock.unlock();
    if (error)
        goto err;

    LayersCv.notify_all();

    return TError::Success();

err:
    TError error2 = layer_tmp.RemoveAll();
    if (error2)
        L_WRN() << "Cannot cleanup layer: " << error2 << std::endl;

    volumes_lock.lock();
    ActivePaths.remove(layer_tmp);
    volumes_lock.unlock();

    LayersCv.notify_all();

    return error;

}

TError RemoveLayer(const std::string &name, const TPath &place) {
    TPath layers = place / PORTO_LAYERS;
    TPath layer = layers / name, layer_tmp;
    TPath layer_private = layers / LAYER_PRIVATE_PREFIX + name;
    TError error;

    error = ValidateLayerName(name);
    if (error)
        return error;

    auto volumes_lock = LockVolumes();

    if (!layer.Exists())
        return TError(EError::LayerNotFound, "Layer " + name + " not found");

    if (LayerInUse(name, place))
        return TError(EError::Busy, "Layer " + name + "in use");

    if (layer_private.Exists()) {
        error = layer_private.Unlink();
        if (error)
            L_WRN() << "Cannot remove layer private: " << error << std::endl;
    }

    layer_tmp = layers / LAYER_REMOVE_PREFIX + std::to_string(LayerRemoveCounter++);
    error = layer.Rename(layer_tmp);
    if (!error)
        ActivePaths.push_back(layer_tmp);

    volumes_lock.unlock();

    if (error)
        return error;

    error = ClearRecursive(layer_tmp);
    if (error)
        L_WRN() << "Cannot clear layel: " << error << std::endl;

    error = layer_tmp.RemoveAll();
    if (error)
        L_WRN() << "Cannot remove layer: " << error << std::endl;

    volumes_lock.lock();
    ActivePaths.remove(layer_tmp);
    volumes_lock.unlock();

    return error;

}

TError GetLayerPrivate(const std::string &name, const TPath &place,
                       std::string &private_value) {
    TPath layers = place / PORTO_LAYERS;
    TPath layer_private = layers / LAYER_PRIVATE_PREFIX + name;
    TPath layer = layers / name;
    TFile private_file;
    TError error;

    private_value = "";

    auto lock = LockVolumes();

    if (layer.Exists()) {
        if (!layer_private.Exists())
            return TError::Success();

        error = private_file.OpenRead(layer_private);
        if (error)
            return error;
    } else {

        return TError(EError::LayerNotFound, "Layer " + name + " not found");
    }

    lock.unlock();

    error = private_file.ReadAll(private_value, 4096);
    if (error)
        return error;

    return TError::Success();
}

TError SetLayerPrivate(const std::string &name, const TPath &place,
                       const std::string private_value) {
    TPath layers = place / PORTO_LAYERS;
    TPath layer_private = layers / LAYER_PRIVATE_PREFIX + name;
    TPath layer = layers / name;
    TFile private_file;
    TError error;

    auto lock = LockVolumes();
    if (layer.Exists()) {
        if (!layer_private.Exists()) {

            error = private_file.CreateNew(layer_private, 0600);
        } else {

            error = private_file.OpenTrunc(layer_private);
        }
    } else {

        return TError(EError::LayerNotFound, "Layer " + name + " not found");
    }

    lock.unlock();

    if (error)
        return error;

    error = private_file.WriteAll(private_value);
    if (error)
        return error;

    return TError::Success();

}

TError SanitizeLayer(TPath layer, bool merge) {
    std::vector<std::string> content;

    TError error = layer.ReadDirectory(content);
    if (error)
        return error;

    for (auto entry: content) {
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
