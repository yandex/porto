#include <algorithm>
#include <layer.hpp>
#include <volume.hpp>
#include <config.hpp>
#include <util/unix.hpp>

extern "C" {
#include <sys/stat.h>
}

TError CheckPlace(const TPath &place, bool init) {
    struct stat st;
    TError error;

    if (!place.IsAbsolute() || !place.IsNormal())
        return TError(EError::InvalidValue, "place path must be normalized");

    TPath volumes = place / config().volumes().volume_dir();
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

    TPath layers = place / config().volumes().layers_dir();
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

    TPath layers_tmp = layers / "_tmp_";
    if (!layers_tmp.IsDirectoryStrict()) {
        (void)layers_tmp.Unlink();
        (void)layers_tmp.Mkdir(0700);
    }

    return TError::Success();
}

TError ValidateLayerName(const std::string &name) {
    auto pos = name.find_first_not_of(PORTO_NAME_CHARS);
    if (pos != std::string::npos)
        return TError(EError::InvalidValue,
                "forbidden character '" + name.substr(pos, 1) + "' in layer name");
    if (name == "." || name == ".."|| name == "_tmp_" )
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

TError ImportLayer(const std::string &name, const TPath &place,
                   const TPath &tarball, bool merge) {
    TPath layers = place / config().volumes().layers_dir();
    TPath layers_tmp = layers / "_tmp_";
    TPath layer = layers / name;
    TPath layer_tmp = layers_tmp / name;
    TError error;

    error = ValidateLayerName(name);
    if (error)
        return error;

    auto volumes_lock = LockVolumes();
    if (layer.Exists()) {
        if (!merge)
            return TError(EError::LayerAlreadyExists, "Layer already exists");
        if (LayerInUse(name, place))
            return TError(EError::Busy, "layer in use");
        error = layer.Rename(layer_tmp);
        if (error)
            return error;
    } else {
        error = layer_tmp.Mkdir(0755);
        if (error)
            return error;
    }
    volumes_lock.unlock();

    error = UnpackTarball(tarball, layer_tmp);
    if (error)
        goto err;

    error = SanitizeLayer(layer_tmp, merge);
    if (error)
        goto err;

    error = layer_tmp.Rename(layer);
    if (error)
        goto err;

    return TError::Success();

err:
    (void)layer_tmp.RemoveAll();
    return error;
}

TError RemoveLayer(const std::string &name, const TPath &place) {
    TPath layers = place / config().volumes().layers_dir();
    TPath layer = layers / name;
    TError error;

    error = ValidateLayerName(name);
    if (error)
        return error;

    if (!layer.Exists())
        return TError(EError::LayerNotFound, "Layer " + name + " not found");

    /* layers_tmp should already be created on startup */
    TPath layers_tmp = layers / "_tmp_";
    TPath layer_tmp = layers_tmp / name;

    auto volumes_lock = LockVolumes();
    if (LayerInUse(name, place))
        error = TError(EError::Busy, "Layer " + name + "in use");
    else
        error = layer.Rename(layer_tmp);
    volumes_lock.unlock();

    if (!error)
        error = layer_tmp.RemoveAll();

    return error;
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
