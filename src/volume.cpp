#include <memory>
#include <sstream>
#include <algorithm>

#include "volume.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/quota.hpp"
#include "util/sha256.hpp"
#include "config.hpp"
#include "kv.pb.h"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
}

/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure() {
    return TError::Success();
}

TError TVolumeBackend::Clear() {
    return Volume->GetPath().ClearDirectory();
}

TError TVolumeBackend::Save() {
    return TError::Success();
}

TError TVolumeBackend::Restore() {
    return TError::Success();
}

TError TVolumeBackend::Resize(uint64_t space_limit, uint64_t inode_limit) {
    return TError(EError::NotSupported, "not implemented");
}

/* TVolumePlainBackend - bindmount */

class TVolumePlainBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (Volume->HaveQuota())
            return TError(EError::NotSupported, "Plain backend have no support of quota");

        return TError::Success();
    }

    TError Build() override {
        TPath storage = Volume->GetStorage();

        TError error = storage.Chown(Volume->VolumeOwner);
        if (error)
            return error;

        error = storage.Chmod(Volume->VolumePerms);
        if (error)
            return error;

        return Volume->GetPath().BindRemount(storage, Volume->GetMountFlags());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        TPath path = Volume->GetPath();
        TError error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;
        return error;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeQuotaBackend - project quota */

class TVolumeQuotaBackend : public TVolumeBackend {
public:

    static bool Supported() {
        static bool supported = false, tested = false;

        if (!config().volumes().enable_quota())
            return false;

        if (!tested) {
            TProjectQuota quota(config().volumes().volume_dir());
            supported = quota.Supported();
            if (supported)
                L_SYS() << "Project quota is supported: " << quota.Path << std::endl;
            else
                L_SYS() << "Project quota not supported: " << quota.Path << std::endl;
            tested = true;
        }

        return supported;
    }

    TError Configure() override {

        if (Volume->IsAutoPath)
            return TError(EError::NotSupported, "Quota backend requires path");

        if (!Volume->HaveQuota())
            return TError(EError::NotSupported, "Quota backend requires space_limit");

        if (Volume->IsReadOnly)
            return TError(EError::NotSupported, "Quota backed doesn't support read_only");

        if (!Volume->IsAutoStorage)
            return TError(EError::NotSupported, "Quota backed doesn't support storage");

        if (Volume->IsLayersSet)
            return TError(EError::NotSupported, "Quota backed doesn't support layers");

        return TError::Success();
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        TProjectQuota quota(path);
        TError error;

        Volume->GetQuota(quota.SpaceLimit, quota.InodeLimit);
        L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
        return quota.Create();
    }

    TError Clear() override {
        return TError(EError::NotSupported, "Quota backend cannot be cleared");
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->GetPath());
        TError error;

        L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
        error = quota.Destroy();
        if (error)
            L_ERR() << "Can't destroy quota: " << error << std::endl;

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->GetPath());

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        return TProjectQuota(Volume->GetPath()).StatFS(result);
    }
};

/* TVolumeNativeBackend - project quota + bindmount */

class TVolumeNativeBackend : public TVolumeBackend {
public:

    static bool Supported() {
        return TVolumeQuotaBackend::Supported();
    }

    TError Configure() override {

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::NotSupported, "project quota is disabled");

        return TError::Success();
    }

    TError Build() override {
        TPath storage = Volume->GetStorage();
        TProjectQuota quota(storage);
        TError error;

        if (Volume->HaveQuota()) {
            Volume->GetQuota(quota.SpaceLimit, quota.InodeLimit);
            L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                    << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
            error = quota.Create();
            if (error)
                return error;
        }

        error = storage.Chown(Volume->VolumeOwner);
        if (error)
            return error;

        error = storage.Chmod(Volume->VolumePerms);
        if (error)
            return error;

        return Volume->GetPath().BindRemount(storage, Volume->GetMountFlags());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->GetStorage());
        TPath path = Volume->GetPath();
        TError error;

        error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;

        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
            error = quota.Destroy();
            if (error)
                L_ERR() << "Can't destroy quota: " << error << std::endl;
        }

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->GetStorage());

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT() << "Creating project quota: " << quota.Path << std::endl;
            return quota.Create();
        }
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        if (Volume->HaveQuota())
            return TProjectQuota(Volume->GetStorage()).StatFS(result);
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeLoopBackend - ext4 image + loop device */

class TVolumeLoopBackend : public TVolumeBackend {
    int LoopDev = -1;

public:

    TPath GetLoopImage() {
        return Volume->GetStorage() / "loop.img";
    }

    TPath GetLoopDevice() {
        if (LoopDev < 0)
            return TPath();
        return TPath("/dev/loop" + std::to_string(LoopDev));
    }

    TError Save() override {
        Volume->LoopDev = LoopDev;

        return TError::Success();
    }

    TError Restore() override {
        LoopDev = Volume->LoopDev;

        return TError::Success();
    }

    static TError MakeImage(const TPath &path, const TCred &cred, off_t size) {
        int fd, status;
        TError error;

        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            return TError(EError::Unknown, errno, "creat(" + path.ToString() + ")");

        if (fchown(fd, cred.Uid, cred.Gid)) {
            error = TError(EError::Unknown, errno, "chown(" + path.ToString() + ")");
            goto remove_file;
        }

        if (fallocate(fd, 0, 0, size) && ftruncate(fd, size)) {
            error = TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");
            goto remove_file;
        }

        close(fd);
        fd = -1;

        error = Run({ "mkfs.ext4", "-F", path.ToString()}, status);
        if (error)
            goto remove_file;

        if (status) {
            error = TError(EError::Unknown, error.GetErrno(),
                    "mkfs.ext4 returned " + std::to_string(status) + ": " + error.GetMsg());
            goto remove_file;
        }

        return TError::Success();

remove_file:
        (void)path.Unlink();
        if (fd >= 0)
            close(fd);

        return error;
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        TPath image = GetLoopImage();
        uint64_t space_limit, inode_limit;
        TError error;

        Volume->GetQuota(space_limit, inode_limit);
        if (!space_limit)
            return TError(EError::InvalidValue, "loop backend requires space_limit");

        if (!image.Exists()) {
            L_ACT() << "Allocate loop image with size " << space_limit << std::endl;
            error = MakeImage(image, Volume->VolumeOwner, space_limit);
            if (error)
                return error;
        } else {
            //FIXME call resize2fs
        }

        error = SetupLoopDevice(image, LoopDev);
        if (error)
            return error;

        error = path.Mount(GetLoopDevice(), "ext4", Volume->GetMountFlags(), {});
        if (error)
            goto free_loop;

        if (!Volume->IsReadOnly) {
            error = path.Chown(Volume->VolumeOwner);
            if (error)
                goto umount_loop;

            error = path.Chmod(Volume->VolumePerms);
            if (error)
                goto umount_loop;
        }

        return TError::Success();

umount_loop:
        (void)path.UmountAll();
free_loop:
        PutLoopDev(LoopDev);
        LoopDev = -1;
        return error;
    }

    TError Destroy() override {
        TPath loop = GetLoopDevice();
        TPath path = Volume->GetPath();

        if (LoopDev < 0)
            return TError::Success();

        L_ACT() << "Destroy loop " << loop << std::endl;
        TError error = path.UmountAll();
        TError error2 = PutLoopDev(LoopDev);
        if (!error)
            error = error2;
        LoopDev = -1;
        return error;
    }

    TError Clear() override {
        return Volume->GetPath().ClearDirectory();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "loop backend doesn't suppport resize");
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeOverlayBackend - project quota + overlayfs */

class TVolumeOverlayBackend : public TVolumeBackend {
public:

    static bool Supported() {
        static bool supported = false, tested = false;

        if (!tested) {
            tested = true;
            if (!mount(NULL, "/", "overlay", MS_SILENT, NULL))
                L_ERR() << "Unexpected success when testing for overlayfs" << std::endl;
            if (errno == EINVAL)
                supported = true;
            else if (errno != ENODEV)
                L_ERR() << "Unexpected errno when testing for overlayfs " << errno << std::endl;
        }

        return supported;
    }

    TError Configure() override {

        if (!Supported())
            return TError(EError::InvalidValue, "overlay not supported");

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::NotSupported, "project quota is disabled");

        return TError::Success();
    }

    TError Build() override {
        TPath storage = Volume->GetStorage();
        TProjectQuota quota(storage);
        TPath upper = storage / "upper";
        TPath work = storage / "work";
        TError error;
        std::stringstream lower;
        int index = 0;

        if (Volume->HaveQuota()) {
            Volume->GetQuota(quota.SpaceLimit, quota.InodeLimit);
            L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                    << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
            error = quota.Create();
            if (error)
                  return error;
        }

        for (auto layer: Volume->GetLayers()) {
            if (index++)
                lower << ":";
            lower << layer;
        }

        if (!upper.Exists()) {
            error = upper.Mkdir(0755);
            if (error)
                goto err;
        }

        error = upper.Chown(Volume->VolumeOwner);
        if (error)
            goto err;

        error = upper.Chmod(Volume->VolumePerms);
        if (error)
            goto err;

        if (!work.Exists()) {
            error = work.Mkdir(0755);
            if (error)
                goto err;
        } else
            work.ClearDirectory();

        error = Volume->GetPath().Mount("overlay", "overlay",
                                        Volume->GetMountFlags(),
                                        { "lowerdir=" + lower.str(),
                                          "upperdir=" + upper.ToString(),
                                          "workdir=" + work.ToString() });
        if (!error)
            return error;
err:
        if (Volume->HaveQuota())
            (void)quota.Destroy();
        return error;
    }

    TError Clear() override {
        return (Volume->GetStorage() / "upper").ClearDirectory();
    }

    TError Destroy() override {
        TPath storage = Volume->GetStorage();
        TProjectQuota quota(storage);
        TPath path = Volume->GetPath();
        TError error, error2;

        error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount overlay: " << error << std::endl;

        if (Volume->IsAutoStorage) {
            error2 = storage.ClearDirectory();
            if (error2) {
                if (!error)
                    error = error2;
                L_ERR() << "Can't clear overlay storage: " << error2 << std::endl;
                (void)(storage / "upper").RemoveAll();
            }
        }

        TPath work = storage / "work";
        if (work.Exists())
            (void)work.RemoveAll();

        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
            error = quota.Destroy();
            if (error)
                L_ERR() << "Can't destroy quota: " << error << std::endl;
        }

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->GetStorage());

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT() << "Creating project quota: " << quota.Path << std::endl;
            return quota.Create();
        }
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        if (Volume->HaveQuota())
            return TProjectQuota(Volume->GetStorage()).StatFS(result);
        return Volume->GetPath().StatFS(result);
    }
};


/* TVolumeRbdBackend - ext4 in ceph rados block device */

class TVolumeRbdBackend : public TVolumeBackend {
    int DeviceIndex = -1;

public:

    std::string GetDevice() {
        if (DeviceIndex < 0)
            return "";
        return "/dev/rbd" + std::to_string(DeviceIndex);
    }

    TError Save() override {
        Volume->LoopDev = DeviceIndex;

        return TError::Success();
    }

    TError Restore() override {
        DeviceIndex = Volume->LoopDev;

        return TError::Success();
    }

    TError MapDevice(std::string id, std::string pool, std::string image,
                     std::string &device) {
        std::vector<std::string> lines;
        L_ACT() << "Map rbd device " << id << "@" << pool << "/" << image << std::endl;
        TError error = Popen("rbd --id=\"" + id + "\" --pool=\"" + pool +
                             "\" map \"" + image + "\"", lines);
        if (error)
            return error;
        if (lines.size() != 1)
            return TError(EError::InvalidValue, "rbd map output have wrong lines count");
        device = StringTrim(lines[0]);
        return TError::Success();
    }

    TError UnmapDevice(std::string device) {
        int status;
        L_ACT() << "Unmap rbd device " << device << std::endl;
        TError error = Run({"rbd", "unmap", device}, status);
        if (!error && status)
            error = TError(EError::Unknown, "rbd unmap " + device +
                                " returned " + std::to_string(status));
        return error;
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        std::string id, pool, image, device;
        std::vector<std::string> tok;
        TError error, error2;

        error = SplitEscapedString(Volume->GetStorage().ToString(), '@', tok);
        if (error)
            return error;
        if (tok.size() != 2)
            return TError(EError::InvalidValue, "Invalid rbd storage");
        id = tok[0];
        image = tok[1];
        tok.clear();
        error = SplitEscapedString(image, '/', tok);
        if (error)
            return error;
        if (tok.size() != 2)
            return TError(EError::InvalidValue, "Invalid rbd storage");
        pool = tok[0];
        image = tok[1];

        error = MapDevice(id, pool, image, device);
        if (error)
            return error;

        if (!StringStartsWith(device, "/dev/rbd")) {
            UnmapDevice(device);
            return TError(EError::InvalidValue, "not rbd device: " + device);
        }

        error = StringToInt(device.substr(8), DeviceIndex);
        if (error) {
            UnmapDevice(device);
            return error;
        }

        error = path.Mount(device, "ext4", Volume->GetMountFlags(), {});
        if (error)
            UnmapDevice(device);
        return error;
    }

    TError Destroy() override {
        std::string device = GetDevice();
        TPath path = Volume->GetPath();
        TError error, error2;

        if (DeviceIndex < 0)
            return TError::Success();

        error = path.UmountAll();
        error2 = UnmapDevice(device);
        if (!error)
            error = error2;
        DeviceIndex = -1;
        return error;
    }

    TError Clear() override {
        return Volume->GetPath().ClearDirectory();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "rbd backend doesn't suppport resize");
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};


/* TVolume */

TError TVolume::OpenBackend() {
    if (BackendType == "plain")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumePlainBackend());
    else if (BackendType == "quota")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeQuotaBackend());
    else if (BackendType == "native")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeNativeBackend());
    else if (BackendType == "overlay")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeOverlayBackend());
    else if (BackendType == "loop")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeLoopBackend());
    else if (BackendType == "rbd")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeRbdBackend());
    else
        return TError(EError::InvalidValue, "Unknown volume backend: " + BackendType);

    Backend->Volume = this;

    return TError::Success();
}

/* /place/porto_volumes/<id>/<type> */
TPath TVolume::GetInternal(std::string type) const {
    return TPath(config().volumes().volume_dir()) / Id / type;
}

/* /chroot/porto/<type>_<id> */
TPath TVolume::GetChrootInternal(TPath container_root, std::string type) const {
    TPath porto_path = container_root / config().container().chroot_porto_dir();
    if (!porto_path.Exists() && porto_path.Mkdir(0755))
        return TPath();
    return porto_path / (type + "_" + Id);
}

TPath TVolume::GetPath() const {
    return Path;
}

TPath TVolume::GetStorage() const {
    if (!IsAutoStorage)
        return TPath(StoragePath);
    else
        return GetInternal(BackendType);
}

unsigned long TVolume::GetMountFlags() const {
    unsigned flags = 0;

    if (IsReadOnly)
        flags |= MS_RDONLY;

    flags |= MS_NODEV | MS_NOSUID;

    return flags;
}

std::vector<TPath> TVolume::GetLayers() const {
    std::vector<TPath> result;

    for (auto layer: Layers) {
        TPath path(layer);
        if (!path.IsAbsolute())
            path = TPath(config().volumes().layers_dir()) / layer;
        result.push_back(path);
    }

    return result;
}

TError TVolume::CheckGuarantee(TVolumeHolder &holder,
        uint64_t space_guarantee, uint64_t inode_guarantee) const {
    auto backend = BackendType;
    TStatFS current, total;
    TPath storage;

    if (backend == "rbd")
        return TError::Success();

    if (!space_guarantee && !inode_guarantee)
        return TError::Success();

    if (IsAutoStorage)
        storage = TPath(config().volumes().volume_dir());
    else
        storage = GetStorage();

    TError error = storage.StatFS(total);
    if (error)
        return error;

    if (!IsReady || StatFS(current))
        current.Reset();

    /* Check available space as is */
    if (total.SpaceAvail + current.SpaceUsage < space_guarantee)
        return TError(EError::NoSpace, "Not enough space for volume guarantee: " +
                      std::to_string(total.SpaceAvail) + " available " +
                      std::to_string(current.SpaceUsage) + " used");

    if (total.InodeAvail + current.InodeUsage < inode_guarantee &&
            backend != "loop")
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used");

    /* Estimate unclaimed guarantees */
    uint64_t space_claimed = 0, space_guaranteed = 0;
    uint64_t inode_claimed = 0, inode_guaranteed = 0;
    for (auto path : holder.ListPaths()) {
        auto volume = holder.Find(path);
        if (volume == nullptr || volume.get() == this ||
                volume->GetStorage().GetDev() != storage.GetDev())
            continue;

        auto volume_backend = volume->BackendType;

        /* rbd stored remotely, plain cannot provide usage */
        if (volume_backend == "rbd" || volume_backend == "plain")
            continue;

        TStatFS stat;
        uint64_t volume_space_guarantee = SpaceGuarantee;
        uint64_t volume_inode_guarantee = InodeGuarantee;

        if (!volume_space_guarantee && !volume_inode_guarantee)
            continue;

        if (!volume->IsReady || volume->StatFS(stat))
            stat.Reset();

        space_guaranteed += volume_space_guarantee;
        if (stat.SpaceUsage < volume_space_guarantee)
            space_claimed += stat.SpaceUsage;
        else
            space_claimed += volume_space_guarantee;

        if (volume_backend != "loop") {
            inode_guaranteed += volume_inode_guarantee;
            if (stat.InodeUsage < volume_inode_guarantee)
                inode_claimed += stat.InodeUsage;
            else
                inode_claimed += volume_inode_guarantee;
        }
    }

    if (total.SpaceAvail + current.SpaceUsage + space_claimed <
            space_guarantee + space_guaranteed)
        return TError(EError::NoSpace, "Not enough space for volume guarantee: " +
                      std::to_string(total.SpaceAvail) + " available " +
                      std::to_string(current.SpaceUsage) + " used " +
                      std::to_string(space_claimed) + " claimed " +
                      std::to_string(space_guaranteed) + " guaranteed");

    if (backend != "loop" &&
            total.InodeAvail + current.InodeUsage + inode_claimed <
            inode_guarantee + inode_guaranteed)
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used " +
                      std::to_string(inode_claimed) + " claimed " +
                      std::to_string(inode_guaranteed) + " guaranteed");

    return TError::Success();
}

TError TVolume::Configure(const TPath &path, const TCred &creator_cred,
                          std::shared_ptr<TContainer> creator_container,
                          const std::map<std::string, std::string> &properties,
                          TVolumeHolder &holder) {
    auto backend = properties.count(V_BACKEND) ? properties.at(V_BACKEND) : "";
    TPath container_root = creator_container->RootPath();
    TError error;

    /* Verify volume path */
    if (!path.IsEmpty()) {
        if (!path.IsAbsolute())
            return TError(EError::InvalidValue, "Volume path must be absolute");
        if (!path.IsNormal())
            return TError(EError::InvalidValue, "Volume path must be normalized");
        if (!path.Exists())
            return TError(EError::InvalidValue, "Volume path does not exist");
        if (!path.IsDirectoryStrict())
            return TError(EError::InvalidValue, "Volume path must be a directory");
        if (!path.CanWrite(creator_cred))
            return TError(EError::Permission, "Volume path usage not permitted");

        Path = path.ToString();

    } else {
        TPath volume_path;

        if (container_root.IsRoot())
            volume_path = GetInternal("volume");
        else
            volume_path = GetChrootInternal(container_root, "volume");
        if (volume_path.IsEmpty())
            return TError(EError::InvalidValue, "Cannot choose automatic volume path");

        Path = volume_path.ToString();
        IsAutoPath = true;
    }

    /* Verify storage path */
    if (backend != "rbd" && properties.count(V_STORAGE)) {
        TPath storage(properties.at(V_STORAGE));
        if (!storage.IsAbsolute())
            return TError(EError::InvalidValue, "Storage path must be absolute");
        if (!storage.IsNormal())
            return TError(EError::InvalidValue, "Storage path must be normalized");
        if (!storage.Exists())
            return TError(EError::InvalidValue, "Storage path does not exist");
        if (!storage.IsDirectoryFollow())
            return TError(EError::InvalidValue, "Storage path must be a directory");
        if (!storage.CanWrite(creator_cred))
            return TError(EError::Permission, "Storage path usage not permitted");

        IsAutoStorage = false;
    }

    /* Save original creator. Just for the record. */
    Creator = creator_container->GetName() + " " + creator_cred.User() + " " +
              creator_cred.Group();

    /* Set default credentials to creator */
    VolumeOwner = creator_cred;

    if (properties.count(V_CREATOR))
        return TError(EError::InvalidProperty,
                      "Setting read-only property: " + std::string(V_CREATOR));

    if (properties.count(V_READY))
        return TError(EError::InvalidProperty,
                      "Setting read-only property: " + std::string(V_READY));

    /* Apply properties */
    error = SetProperty(properties);
    if (error)
        return error;

    /* Verify default credentials */
    if (VolumeOwner.Uid != creator_cred.Uid && !creator_cred.IsRootUser())
        return TError(EError::Permission, "Changing user is not permitted");

    if (VolumeOwner.Gid != creator_cred.Gid && !creator_cred.IsRootUser() &&
            !creator_cred.IsMemberOf(VolumeOwner.Gid))
        return TError(EError::Permission, "Changing group is not permitted");

    /* Verify and resolve layers */
    if (IsLayersSet) {
        std::vector <std::string> layers;

        for (auto &l: Layers) {
            TPath layer(l);
            if (!layer.IsNormal())
                return TError(EError::InvalidValue, "Layer path must be normalized");
            if (layer.IsAbsolute()) {
                layer = container_root / layer;
                l = layer.ToString();
                if (!layer.Exists())
                    return TError(EError::LayerNotFound, "Layer not found");
                if (!layer.CanWrite(creator_cred))
                    return TError(EError::Permission, "Layer path not permitted");
            } else {
                if (l.find('/') != std::string::npos)
                    return TError(EError::InvalidValue, "Internal layer storage has no directories");
                layer = TPath(config().volumes().layers_dir()) / layer;
            }
            if (!layer.Exists())
                return TError(EError::LayerNotFound, "Layer not found");
            if (!layer.IsDirectoryFollow())
                return TError(EError::InvalidValue, "Layer must be a directory");
        }
    }

    /* Verify guarantees */
    if (properties.count(V_SPACE_LIMIT) && properties.count(V_SPACE_GUARANTEE) &&
            SpaceLimit < SpaceGuarantee)
        return TError(EError::InvalidValue, "Space guarantree bigger than limit");

    if (properties.count(V_INODE_LIMIT) && properties.count(V_INODE_GUARANTEE) &&
            InodeLimit < InodeGuarantee)
        return TError(EError::InvalidValue, "Inode guarantree bigger than limit");

    /* Autodetect volume backend */
    if (!properties.count(V_BACKEND)) {
        if (HaveQuota() && !TVolumeNativeBackend::Supported())
            BackendType = "loop";
        else if (IsLayersSet && TVolumeOverlayBackend::Supported())
            BackendType = "overlay";
        else if (TVolumeNativeBackend::Supported())
            BackendType = "native";
        else
            BackendType = "plain";
        if (error)
            return error;
    }

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Configure();
    if (error)
        return error;

    error = CheckGuarantee(holder, SpaceGuarantee, InodeGuarantee);
    if (error)
        return error;

    return TError::Success();
}

TError TVolume::Build() {
    TPath storage = GetStorage();
    TPath path = Path;
    TPath internal = GetInternal("");

    L_ACT() << "Build volume: " << path
            << " backend: " << BackendType << std::endl;

    TError error = internal.Mkdir(0755);
    if (error)
        goto err_internal;

    if (IsAutoStorage) {
        error = storage.Mkdir(0755);
        if (error)
            goto err_storage;
    }

    if (IsAutoPath) {
        error = path.Mkdir(0755);
        if (error)
            goto err_path;
    }

    error = Backend->Build();
    if (error)
        goto err_build;

    error = Backend->Save();
    if (error)
        goto err_save;

    if (IsLayersSet && BackendType != "overlay") {
        L_ACT() << "Merge layers into volume: " << path << std::endl;

        auto layers = GetLayers();
        for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
            error = CopyRecursive(*layer, path);
            if (error)
                goto err_merge;
        }

        error = SanitizeLayer(path, true);
        if (error)
            goto err_merge;

        error = path.Chown(VolumeOwner);
        if (error)
            return error;

        error = path.Chmod(VolumePerms);
        if (error)
            return error;
    }

    return Save();

err_merge:
err_save:
    (void)Backend->Destroy();
err_build:
    if (IsAutoPath) {
        (void)path.RemoveAll();
    }
err_path:
    if (IsAutoStorage)
        (void)storage.RemoveAll();
err_storage:
    (void)internal.RemoveAll();
err_internal:
    return error;
}

TError TVolume::Clear() {
    L_ACT() << "Clear volume: " << GetPath() << std::endl;
    return Backend->Clear();
}

TError TVolume::Destroy(TVolumeHolder &holder) {
    TPath internal = GetInternal("");
    TPath storage = GetStorage();
    TError ret = TError::Success(), error;

    L_ACT() << "Destroy volume: " << GetPath()
            << " backend: " << BackendType << std::endl;

    if (Backend) {
        error = Backend->Destroy();
        if (error) {
            L_ERR() << "Can't destroy volume backend: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (IsAutoStorage && storage.Exists()) {
        error = storage.RemoveAll();
        if (error) {
            L_ERR() << "Can't remove storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (IsAutoPath && GetPath().Exists()) {
        error = GetPath().RemoveAll();
        if (error) {
            L_ERR() << "Can't remove volume path: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (internal.Exists()) {
        error = internal.RemoveAll();
        if (error) {
            L_ERR() << "Can't remove internal: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (IsLayersSet) {
        for (auto &layer: Layers) {
            if (StringStartsWith(layer, "_weak_")) {
                error = holder.RemoveLayer(layer);
                if (error && error.GetError() != EError::Busy)
                    L_ERR() << "Cannot remove layer: " << error << std::endl;
            }
        }

        Layers.clear();
    }


    auto kvnode = Storage->GetNode(Id);
    error = kvnode->Remove();
    if (!ret && error)
        ret = error;

    return ret;
}

TError TVolume::StatFS(TStatFS &result) const {
    return Backend->StatFS(result);
}

TError TVolume::Tune(TVolumeHolder &holder, const std::map<std::string,
                     std::string> &properties) {

    for (auto &p : properties) {
        if (p.first != V_INODE_LIMIT ||
            p.first != V_INODE_GUARANTEE ||
            p.first != V_SPACE_LIMIT ||
            p.first != V_SPACE_GUARANTEE)
            /* Prop not found omitted */
                return TError(EError::InvalidProperty,
                              "Volume property " + p.first + " cannot be changed");
    }

    TError error;

    if (properties.count(V_SPACE_LIMIT) || properties.count(V_INODE_LIMIT)) {
        uint64_t spaceLimit = SpaceLimit, inodeLimit = InodeLimit;

        if (properties.count(V_SPACE_LIMIT)) {
            error = StringToSize(properties.at(V_SPACE_LIMIT), spaceLimit);
            if (error)
                return error;
        }
        if (properties.count(V_INODE_LIMIT)) {
            error = StringToSize(properties.at(V_INODE_LIMIT), inodeLimit);
            if (error)
                return error;
        }

        error = Resize(spaceLimit, inodeLimit);
    }

    if (properties.count(V_SPACE_GUARANTEE) || properties.count(V_INODE_GUARANTEE)) {
        uint64_t space_guarantee = SpaceGuarantee, inode_guarantee = InodeGuarantee;

        if (properties.count(V_SPACE_GUARANTEE)) {
            error = StringToSize(properties.at(V_SPACE_GUARANTEE), space_guarantee);
            if (error)
                return error;
        }
        if (properties.count(V_INODE_GUARANTEE)) {
            error = StringToSize(properties.at(V_INODE_GUARANTEE), inode_guarantee);
            if (error)
                return error;
        }

        auto lock = holder.ScopedLock();
        error = CheckGuarantee(holder, space_guarantee, inode_guarantee);
        if (error)
            return error;

        SpaceGuarantee = space_guarantee;
        InodeGuarantee = inode_guarantee;
    }

    return Save();
}

TError TVolume::Resize(uint64_t space_limit, uint64_t inode_limit) {
    L_ACT() << "Resize volume: " << GetPath() << " to bytes: "
            << space_limit << " inodes: " << inode_limit << std::endl;

    TError error = Backend->Resize(space_limit, inode_limit);
    if (error)
        return error;

    SpaceLimit = space_limit;
    InodeLimit = inode_limit;

    return Save();
}

TError TVolume::GetUpperLayer(TPath &upper) {
    if (BackendType == "overlay")
        upper = GetStorage() / "upper";
    else
        upper = Path;
    return TError::Success();
}

TError TVolume::LinkContainer(std::string name) {
    Containers.push_back(name);

    return Save();
}

bool TVolume::UnlinkContainer(std::string name) {
    Containers.erase(std::remove(Containers.begin(), Containers.end(), name),
                     Containers.end());

    (void)Save();

    return Containers.empty();
}

std::map<std::string, std::string> TVolume::GetProperties(TPath container_root) {
    std::map<std::string, std::string> ret;
    TStatFS stat;

    if (IsReady && !StatFS(stat)) {
        ret[V_SPACE_USED] = std::to_string(stat.SpaceUsage);
        ret[V_INODE_USED] = std::to_string(stat.InodeUsage);
        ret[V_SPACE_AVAILABLE] = std::to_string(stat.SpaceAvail);
        ret[V_INODE_AVAILABLE] = std::to_string(stat.InodeAvail);
    }

    /* Let's skip HasValue for now */

    ret[V_STORAGE] = StoragePath;
    ret[V_BACKEND] = BackendType;
    ret[V_USER] = VolumeOwner.User();
    ret[V_GROUP] = VolumeOwner.Group();
    ret[V_PERMISSIONS] = StringFormat("%#o", VolumePerms);
    ret[V_CREATOR] = Creator;
    ret[V_READY] = IsReady ? "true" : "false";
    ret[V_PRIVATE] = Private;
    ret[V_READ_ONLY] = IsReadOnly ? "true" : "false";
    ret[V_SPACE_LIMIT] = std::to_string(SpaceLimit);
    ret[V_INODE_LIMIT] = std::to_string(InodeLimit);
    ret[V_SPACE_GUARANTEE] = std::to_string(SpaceGuarantee);
    ret[V_INODE_GUARANTEE] = std::to_string(InodeGuarantee);

    if (IsLayersSet) {
        std::vector<std::string> layers = Layers;

        for (auto &l: layers) {
            TPath path(l);
            if (path.IsAbsolute())
                l = container_root.InnerPath(path).ToString();
        }
        ret[V_LAYERS] = MergeEscapeStrings(layers, ";", "\\;");
    }

    return ret;
}

TError TVolume::CheckPermission(const TCred &ucred) const {
    if (ucred.IsPermitted(VolumeOwner))
        return TError::Success();

    return TError(EError::Permission, "Permission denied");
}

static void SetNode(kv::TNode &node, std::string key, std::string value) {
    auto pair = node.add_pairs();

    pair->set_key(key);
    pair->set_val(value);
}

TError TVolume::Save() {
    auto kvnode = Storage->GetNode(Id);
    kv::TNode node;
    TError error;
    std::string tmp;

    kvnode->Create();

    /*
     * Storing all state values on save,
     * the previous scheme stored knobs selectively.
     */

    SetNode(node, V_ID, Id);
    SetNode(node, V_PATH, Path);
    SetNode(node, V_AUTO_PATH, IsAutoPath ? "true" : "false");
    SetNode(node, V_STORAGE, StoragePath);
    SetNode(node, V_BACKEND, BackendType);
    SetNode(node, V_USER, VolumeOwner.User());
    SetNode(node, V_GROUP, VolumeOwner.Group());
    SetNode(node, V_PERMISSIONS, StringFormat("%#o", VolumePerms));
    SetNode(node, V_CREATOR, Creator);
    SetNode(node, V_READY, IsReady ? "true" : "false");
    SetNode(node, V_PRIVATE, Private);

    error = StrListToString(Containers, tmp);
    if (error)
        return error;

    SetNode(node, V_CONTAINERS, tmp);
    SetNode(node, V_LOOP_DEV, std::to_string(LoopDev));
    SetNode(node, V_READ_ONLY, IsReadOnly ? "true" : "false");

    error = StrListToString(Layers, tmp);
    if (error)
        return error;

    SetNode(node, V_LAYERS, tmp);
    SetNode(node, V_SPACE_LIMIT, std::to_string(SpaceLimit));
    SetNode(node, V_SPACE_GUARANTEE, std::to_string(SpaceGuarantee));
    SetNode(node, V_INODE_LIMIT, std::to_string(InodeLimit));
    SetNode(node, V_INODE_GUARANTEE, std::to_string(InodeGuarantee));

    return kvnode->Append(node);
}

TError TVolume::Restore(const kv::TNode &node) {
    std::map<std::string, std::string> props;

    for (int i = 0; i < node.pairs_size(); i++) {
        std::string key = node.pairs(i).key();
        std::string value = node.pairs(i).val();

        props[key] = value;
    }

    if (!props.count(V_ID))
        return TError(EError::InvalidValue, "No volume id stored");

    TError error = SetProperty(props);
    if (error)
        return error;

    if (!IsReady)
        return TError(EError::Busy, "Volume not ready");

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Restore();
    if (error)
        return error;

    return TError::Success();
}

/* TVolumeHolder */

const std::vector<std::pair<std::string, std::string>> TVolumeHolder::ListProperties() {
    return {
        { V_BACKEND,     "plain|quota|native|overlay|loop|rbd (default - autodetect)" },
        { V_STORAGE,     "path to data storage (default - internal)" },
        { V_READY,       "true|false - contruction complete (ro)" },
        { V_PRIVATE,     "user-defined property" },
        { V_USER,        "user (default - creator)" },
        { V_GROUP,       "group (default - creator)" },
        { V_PERMISSIONS, "directory permissions (default - 0775)" },
        { V_CREATOR,     "container user group (ro)" },
        { V_READ_ONLY,   "true|false (default - false)" },
        { V_LAYERS,      "top-layer;...;bottom-layer - overlayfs layers" },
        { V_SPACE_LIMIT, "disk space limit (dynamic, default zero - unlimited)" },
        { V_INODE_LIMIT, "disk inode limit (dynamic, default zero - unlimited)"},
        { V_SPACE_GUARANTEE,    "disk space guarantee (dynamic, default - zero)" },
        { V_INODE_GUARANTEE,    "disk inode guarantee (dynamic, default - zero)" },
        { V_SPACE_USED,  "current disk space usage (ro)" },
        { V_INODE_USED,  "current disk inode used (ro)" },
        { V_SPACE_AVAILABLE,    "available disk space (ro)" },
        { V_INODE_AVAILABLE,    "available disk inodes (ro)" },
    };
}

TError TVolumeHolder::Create(std::shared_ptr<TVolume> &volume) {
    volume = std::make_shared<TVolume>(Storage);
    volume->Id = std::to_string(NextId);

    NextId++;

    return TError::Success();
}

void TVolumeHolder::Remove(std::shared_ptr<TVolume> volume) {}

TError TVolumeHolder::RestoreFromStorage(std::shared_ptr<TContainerHolder> Cholder) {
    std::vector<std::shared_ptr<TKeyValueNode>> list;
    TError error;

    TPath volumes = config().volumes().volume_dir();
    if (!volumes.IsDirectoryFollow()) {
        (void)volumes.Unlink();
        error = volumes.MkdirAll(0755);
        if (error)
            return error;
    }

    TPath layers = config().volumes().layers_dir();
    if (!layers.IsDirectoryFollow()) {
        (void)layers.Unlink();
        error = layers.MkdirAll(0700);
        if (error)
            return error;
    }

    TPath layers_tmp = layers / "_tmp_";
    if (layers_tmp.Exists()) {
        L_ACT() << "Remove stale layers..." << std::endl;
        error = layers_tmp.ClearDirectory();
        if (error)
            L_ERR() << "Cannot remove stale layers: " << error << std::endl;
    } else {
        error = layers_tmp.Mkdir(0700);
        if (error)
            return error;
    }

    error = Storage->ListNodes(list);
    if (error)
        return error;

    for (auto &node : list) {
        L_ACT() << "Restore volume: " << node->Name << std::endl;

        auto volume = std::make_shared<TVolume>(Storage);
        kv::TNode n;
        error = node->Load(n);
        if (error)
            return error;

        error = volume->Restore(n);
        if (error) {
            L_WRN() << "Corrupted volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy(*this);
            (void)Remove(volume);
            continue;
        }

        uint64_t id;
        if (!StringToUint64(volume->Id, id)) {
            if (id >= NextId)
                NextId = id + 1;
        }

        error = Register(volume);
        if (error) {
            L_WRN() << "Cannot register volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy(*this);
            (void)Remove(volume);
            continue;
        }

        for (auto name: volume->GetContainers()) {
            std::shared_ptr<TContainer> container;
            if (!Cholder->Get(name, container)) {
                container->VolumeHolder = shared_from_this();
                container->Volumes.emplace_back(volume);
            } else if (!volume->UnlinkContainer(name)) {
                (void)volume->Destroy(*this);
                (void)Unregister(volume);
                (void)Remove(volume);

                L_WRN() << "Cannot unlink volume " << volume->GetPath() <<
                           "from container " << name << std::endl; 

                continue;
            }
        }

        error = volume->Save();
        if (error) {
            (void)volume->Destroy(*this);
            (void)Unregister(volume);
            (void)Remove(volume);

            continue;
        }

        L() << "Volume " << volume->GetPath() << " restored" << std::endl;
    }

    L_ACT() << "Remove stale volumes..." << std::endl;

    std::vector<std::string> subdirs;
    error = volumes.ReadDirectory(subdirs);
    if (error)
        L_ERR() << "Cannot list " << volumes << std::endl;

    for (auto dir_name: subdirs) {
        bool used = false;
        for (auto v: Volumes) {
            if (v.second->Id == dir_name) {
                used = true;
                break;
            }
        }
        if (used)
            continue;

        TPath dir = volumes / dir_name;
        TPath mnt = dir / "volume";
        if (mnt.Exists()) {
            error = mnt.UmountAll();
            if (error)
                L_ERR() << "Cannot umount volume " << mnt << ": " << error << std::endl;
        }
        error = dir.RemoveAll();
        if (error)
            L_ERR() << "Cannot remove directory " << dir << std::endl;
    }

    return TError::Success();
}

void TVolumeHolder::Destroy() {
    while (Volumes.begin() != Volumes.end()) {
        auto name = Volumes.begin()->first;
        auto volume = Volumes.begin()->second;
        TError error = volume->Destroy(*this);
        if (error)
            L_ERR() << "Can't destroy volume " << name << ": " << error << std::endl;
        Unregister(volume);
        Remove(volume);
    }
}

TError TVolumeHolder::Register(std::shared_ptr<TVolume> volume) {
    if (Volumes.find(volume->GetPath()) == Volumes.end()) {
        Volumes[volume->GetPath()] = volume;
        return TError::Success();
    }

    return TError(EError::VolumeAlreadyExists, "Volume already exists");
}

void TVolumeHolder::Unregister(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetPath());
}

std::shared_ptr<TVolume> TVolumeHolder::Find(const TPath &path) {
    auto v = Volumes.find(path);
    if (v != Volumes.end())
        return v->second;
    else
        return nullptr;
}

std::vector<TPath> TVolumeHolder::ListPaths() const {
    std::vector<TPath> ret;

    for (auto v : Volumes)
        ret.push_back(v.first);

    return ret;
}

bool TVolumeHolder::LayerInUse(TPath layer) {
    for (auto &volume : Volumes) {
        for (auto &l: volume.second->GetLayers()) {
            if (l.NormalPath() == layer)
                return true;
        }
    }
    return false;
}

TError TVolumeHolder::RemoveLayer(const std::string &name) {
    TPath layers = TPath(config().volumes().layers_dir());
    TPath layer = layers / name;
    TError error;

    if (!layer.Exists())
        return TError(EError::LayerNotFound, "Layer " + name + " not found");

    /* layers_tmp should already be created on startup */
    TPath layers_tmp = layers / "_tmp_";
    TPath layer_tmp = layers_tmp / name;

    auto lock = ScopedLock();
    if (LayerInUse(layer))
        error = TError(EError::Busy, "Layer " + name + "in use");
    else
        error = layer.Rename(layer_tmp);
    lock.unlock();

    if (!error)
        error = layer_tmp.RemoveAll();

    return error;
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

TError TVolume::SetProperty(const std::map<std::string, std::string> &properties) {
    TError error;

    for (auto &prop : properties) {

        L_ACT() << "Volume restoring : " << prop.first << " : " << prop.second << std::endl;

        if (prop.first == V_PATH) {
            Path = prop.second;

        } else if (prop.first == V_AUTO_PATH) {
            if (prop.second == "true")
                IsAutoPath = true;
            else if (prop.second == "false")
                IsAutoPath = false;
            else
                return TError(EError::InvalidValue, "Invalid bool value");

        } else if (prop.first == V_STORAGE) {
            StoragePath = prop.second;

        } else if (prop.first == V_BACKEND) {
            BackendType = prop.second;

        } else if (prop.first == V_USER) {
            error = UserId(prop.second, VolumeOwner.Uid);
            if (error)
                return error;

        } else if (prop.first == V_GROUP) {
            error = GroupId(prop.second, VolumeOwner.Gid);
            if (error)
                return error;

        } else if (prop.first == V_PERMISSIONS) {
            error = StringToOct(prop.second, VolumePerms);
            if (error)
                return error;

        } else if (prop.first == V_CREATOR) {
            Creator = prop.second;

        } else if (prop.first == V_ID) {
            Id = prop.second;

        } else if (prop.first == V_READY) {
            if (prop.second == "true")
                IsReady = true;
            else if (prop.second == "false")
                IsReady = false;
            else
                return TError(EError::InvalidValue, "Invalid bool value");

        } else if (prop.first == V_PRIVATE) {
            Private = prop.second;

        } else if (prop.first == V_CONTAINERS) {
            error = StringToStrList(prop.second, Containers);
            if (error)
                return error;

        } else if (prop.first == V_LOOP_DEV) {
            error = StringToInt(prop.second, LoopDev);
            if (error)
                return error;

        } else if (prop.first == V_READ_ONLY) {
            if (prop.second == "true")
                IsReadOnly = true;
            else if (prop.second == "false")
                IsReadOnly = false;
            else
                return TError(EError::InvalidValue, "Invalid bool value");

        } else if (prop.first == V_LAYERS) {
            error = StringToStrList(prop.second, Layers);
            if (error)
                return error;

            IsLayersSet = true;

        } else if (prop.first == V_SPACE_LIMIT) {
            uint64_t limit;
            error = StringToSize(prop.second, limit);
            if (error)
                return error;

            SpaceLimit = limit;

        } else if (prop.first == V_SPACE_GUARANTEE) {
            uint64_t guarantee;
            error = StringToSize(prop.second, guarantee);
            if (error)
                return error;

            SpaceGuarantee = guarantee;

        } else if (prop.first == V_INODE_LIMIT) {
            uint64_t limit;
            error = StringToSize(prop.second, limit);
            if (error)
                return error;

            InodeLimit = limit;

        } else if (prop.first == V_INODE_GUARANTEE) {
            uint64_t guarantee;
            error = StringToSize(prop.second, guarantee);
            if (error)
                return error;

            InodeGuarantee = guarantee;

        } else {
            return TError(EError::InvalidValue, "Invalid value name: " + prop.first);
        }
    }

    return TError::Success();
}
