#include <sstream>
#include <algorithm>

#include "volume.hpp"
#include "layer.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/quota.hpp"
#include "util/loop.hpp"
#include "config.hpp"
#include "kvalue.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <linux/falloc.h>
}

TPath VolumesKV;
std::mutex VolumesMutex;
std::map<TPath, std::shared_ptr<TVolume>> Volumes;
static uint64_t NextId = 1;

/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure() {
    return TError::Success();
}

TError TVolumeBackend::Clear() {
    return Volume->Path.ClearDirectory();
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

        return Volume->Path.BindRemount(storage, Volume->GetMountFlags());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        TPath path = Volume->Path;
        TError error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;
        return error;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->Path.StatFS(result);
    }
};

/* TVolumeBindBackend - bind mount */

class TVolumeBindBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (!Volume->HaveStorage())
            return TError(EError::NotSupported, "bind backed require storage");

        if (Volume->HaveQuota())
            return TError(EError::NotSupported, "bind backend doesn't support quota");

        if (Volume->HaveLayers())
            return TError(EError::NotSupported, "bind backend doesn't support layers");

        return TError::Success();
    }

    TError Build() override {
        return Volume->Path.BindRemount(Volume->GetStorage(), Volume->GetMountFlags());
    }

    TError Destroy() override {
        return Volume->Path.UmountAll();
    }

    TError StatFS(TStatFS &result) override {
        return Volume->Path.StatFS(result);
    }
};

/* TVolumeTmpfsBackend - tmpfs */

class TVolumeTmpfsBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (!Volume->HaveQuota())
            return TError(EError::NotSupported, "tmpfs backend requires space_limit");

        if (Volume->HaveStorage())
            return TError(EError::NotSupported, "tmpfs backed doesn't support storage");

        return TError::Success();
    }

    TError Build() override {
        return Volume->Path.Mount("porto:" + Volume->Id, "tmpfs",
                Volume->GetMountFlags(),
                { "size=" + std::to_string(Volume->SpaceLimit),
                  "uid=" + std::to_string(Volume->VolumeOwner.Uid),
                  "gid=" + std::to_string(Volume->VolumeOwner.Gid),
                  "mode=" + StringFormat("%#o", Volume->VolumePerms)
                });
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return Volume->Path.Mount("porto:" + Volume->Id, "tmpfs",
                Volume->GetMountFlags() | MS_REMOUNT,
                { "size=" + std::to_string(space_limit),
                  "uid=" + std::to_string(Volume->VolumeOwner.Uid),
                  "gid=" + std::to_string(Volume->VolumeOwner.Gid),
                  "mode=" + StringFormat("%#o", Volume->VolumePerms)
                });
    }

    TError Destroy() override {
        TPath path = Volume->Path;
        TError error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;
        return error;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->Path.StatFS(result);
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
            TProjectQuota quota(config().volumes().default_place() + "/" + config().volumes().volume_dir());
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

        if (Volume->HaveStorage())
            return TError(EError::NotSupported, "Quota backed doesn't support storage");

        if (Volume->HaveLayers())
            return TError(EError::NotSupported, "Quota backed doesn't support layers");

        return TError::Success();
    }

    TError Build() override {
        TPath path = Volume->Path;
        TProjectQuota quota(path);
        TError error;

        quota.SpaceLimit = Volume->SpaceLimit;
        quota.InodeLimit = Volume->InodeLimit;
        L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
        return quota.Create();
    }

    TError Clear() override {
        return TError(EError::NotSupported, "Quota backend cannot be cleared");
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->Path);
        TError error;

        L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
        error = quota.Destroy();
        if (error)
            L_ERR() << "Can't destroy quota: " << error << std::endl;

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->Path);

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        return TProjectQuota(Volume->Path).StatFS(result);
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
            quota.SpaceLimit = Volume->SpaceLimit;
            quota.InodeLimit = Volume->InodeLimit;
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

        return Volume->Path.BindRemount(storage, Volume->GetMountFlags());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->GetStorage());
        TPath path = Volume->Path;
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
        return Volume->Path.StatFS(result);
    }
};

/* TVolumeLoopBackend - ext4 image + loop device */

class TVolumeLoopBackend : public TVolumeBackend {
    int LoopDev = -1;

public:

    TPath GetLoopImage() {
        TPath storage = Volume->GetStorage();

        if (Volume->IsFileStorage)
            return storage;

        return storage / "loop.img";
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

    static TError MakeImage(const TPath &path, const TCred &cred, off_t size, off_t guarantee) {
        TError error;
        TFile image;

        error = image.CreateNew(path, 0644);
        if (error)
            return error;

        if (fchown(image.Fd, cred.Uid, cred.Gid)) {
            error = TError(EError::Unknown, errno, "chown(" + path.ToString() + ")");
            goto remove_file;
        }

        if (ftruncate(image.Fd, size)) {
            error = TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");
            goto remove_file;
        }

        if (guarantee && fallocate(image.Fd, FALLOC_FL_KEEP_SIZE, 0, guarantee)) {
            error = TError(EError::ResourceNotAvailable, errno,
                           "cannot fallocate guarantee " + std::to_string(guarantee));
            goto remove_file;
        }

        image.Close();

        error = RunCommand({ "mkfs.ext4", "-F", "-m", "0", "-E", "nodiscard",
                             "-O", "^has_journal", path.ToString()}, path.DirName());
        if (error)
            goto remove_file;

        return TError::Success();

remove_file:
        (void)path.Unlink();
        return error;
    }

    TError Build() override {
        TPath path = Volume->Path;
        TPath image = GetLoopImage();
        TError error;

        if (!image.Exists()) {
            if (!Volume->SpaceLimit)
                return TError(EError::InvalidValue, "loop backend requires space_limit");

            L_ACT() << "Allocate loop image with size " << Volume->SpaceLimit
                    << " guarantee " << Volume->SpaceGuarantee << std::endl;
            error = MakeImage(image, Volume->VolumeOwner,
                              Volume->SpaceLimit, Volume->SpaceGuarantee);
            if (error)
                return error;

        } else {
            struct stat st;

            error = image.StatFollow(st);
            if (error)
                return error;

            if (!Volume->SpaceLimit) {
                Volume->SpaceLimit = st.st_size;

            } else {
                //FIXME: call resize2fs

            }
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
        TPath path = Volume->Path;

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
        return Volume->Path.ClearDirectory();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "loop backend doesn't suppport resize");
    }

    TError StatFS(TStatFS &result) override {
        return Volume->Path.StatFS(result);
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
        int layer_idx = 0;

        if (Volume->HaveQuota()) {
            quota.SpaceLimit = Volume->SpaceLimit;
            quota.InodeLimit = Volume->InodeLimit;
            L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                    << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
            error = quota.Create();
            if (error)
                  return error;
        }

        for (auto &name: Volume->Layers) {
            TPath path, temp;
            TFile pin;

            if (name[0] == '/') {
                error = pin.OpenDir(name);
                if (Volume->CreatorRoot.InnerPath(pin.RealPath()).IsEmpty()) {
                    error = TError(EError::Permission, "Layer path outside root: " + name);
                    goto err;
                }
                path = pin.ProcPath();
                if (!path.CanWrite(Volume->CreatorCred)) {
                    error = TError(EError::Permission, "Layer path not permitted: " + name);
                    goto err;
                }
            } else
                path = Volume->Place / config().volumes().layers_dir() / name;

            temp = Volume->GetInternal("layer_" + std::to_string(layer_idx++));
            error = temp.Mkdir(700);
            if (!error)
                error = temp.BindRemount(path, MS_RDONLY | MS_NODEV);
            if (!error)
                error = temp.Remount(MS_PRIVATE);
            if (error)
                goto err;

            pin.Close();

            if (layer_idx > 1)
                lower << ":";
            lower << StringReplaceAll(temp.ToString(), ":", "\\:");
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

        error = Volume->Path.Mount("overlay", "overlay",
                                        Volume->GetMountFlags(),
                                        { "lowerdir=" + lower.str(),
                                          "upperdir=" + upper.ToString(),
                                          "workdir=" + work.ToString() });
err:
        while (layer_idx--) {
            TPath temp = Volume->GetInternal("layer_" + std::to_string(layer_idx));
            (void)temp.UmountAll();
            (void)temp.Rmdir();
        }

        if (!error)
            return error;

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
        TPath path = Volume->Path;
        TError error, error2;

        error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount overlay: " << error << std::endl;

        if (!Volume->HaveStorage()) {
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
        return Volume->Path.StatFS(result);
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
        L_ACT() << "Unmap rbd device " << device << std::endl;
        return RunCommand({"rbd", "unmap", device}, "/");
    }

    TError Build() override {
        TPath path = Volume->Path;
        std::string id, pool, image, device;
        std::vector<std::string> tok;
        TError error, error2;

        SplitEscapedString(Volume->GetStorage().ToString(), tok, '@');
        if (tok.size() != 2)
            return TError(EError::InvalidValue, "Invalid rbd storage");
        id = tok[0];
        image = tok[1];
        tok.clear();
        SplitEscapedString(image, tok, '/');
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
        TPath path = Volume->Path;
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
        return Volume->Path.ClearDirectory();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "rbd backend doesn't suppport resize");
    }

    TError StatFS(TStatFS &result) override {
        return Volume->Path.StatFS(result);
    }
};


/* TVolume */

std::shared_ptr<TVolume> TVolume::Find(const TPath &path) {
    auto volumes_lock = LockVolumes();
    auto it = Volumes.find(path);
    if (it == Volumes.end())
        return nullptr;
    return it->second;
}

TError TVolume::Find(const TPath &path, std::shared_ptr<TVolume> &volume) {
    volume = Find(path);
    if (volume)
        return TError::Success();
    return TError(EError::VolumeNotFound, "Volume " + path.ToString() + " not found");
}

TError TVolume::OpenBackend() {
    if (BackendType == "plain")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumePlainBackend());
    else if (BackendType == "bind")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeBindBackend());
    else if (BackendType == "tmpfs")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeTmpfsBackend());
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
    return Place / config().volumes().volume_dir() / Id / type;
}

TPath TVolume::GetStorage(void) const {
    if (HaveStorage())
        return TPath(Storage);
    return GetInternal(BackendType);
}

unsigned long TVolume::GetMountFlags(void) const {
    unsigned flags = 0;

    if (IsReadOnly)
        flags |= MS_RDONLY;

    flags |= MS_NODEV | MS_NOSUID;

    return flags;
}

TError TVolume::CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee) const {
    auto backend = BackendType;
    TStatFS current, total;
    TPath storage;

    if (backend == "rbd" || backend == "tmpfs")
        return TError::Success();

    if (!space_guarantee && !inode_guarantee)
        return TError::Success();

    if (!HaveStorage())
        storage = Place / config().volumes().volume_dir();
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
    for (auto &it: Volumes) {
        auto volume = it.second;
        if (volume.get() == this ||
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

TError TVolume::Configure(const TPath &path, const TStringMap &cfg,
                          const TContainer &container, const TCred &cred) {
    auto backend = cfg.count(V_BACKEND) ? cfg.at(V_BACKEND) : "";
    TError error;

    /* Verify properties */
    for (auto &it: cfg) {
        TVolumeProperty *prop = nullptr;
        for (auto &p: VolumeProperties) {
            if (p.Name == it.first) {
                prop = &p;
                break;
            }
        }
        if (!prop)
            return TError(EError::InvalidProperty, "Unknown: " + it.first);
        if (prop->ReadOnly)
            return TError(EError::InvalidProperty, "Read-only: " + it.first);
    }

    /* Verify place */
    if (cfg.count(V_PLACE)) {
        Place = cfg.at(V_PLACE);
        error = CheckPlace(Place);
        if (error)
            return error;
        CustomPlace = true;
    } else {
        Place = config().volumes().default_place();
        CustomPlace = false;
    }

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
        if (!path.CanWrite(cred))
            return TError(EError::Permission, "Volume path usage not permitted");

        Path = path.ToString();

    } else {
        if (container.RootPath.IsRoot()) {
            /* /place/porto_volumes/<id>/volume */
            Path = GetInternal("volume");
        } else {
            /* /chroot/porto/volume_<id> */
            TPath porto_path = container.RootPath / config().container().chroot_porto_dir();
            if (!porto_path.Exists()) {
                error = porto_path.Mkdir(0755);
                if (error)
                    return error;
            }
            Path = porto_path / "volume_" + Id;
        }
        IsAutoPath = true;
    }

    /* Save original creator. Just for the record. */
    Creator = container.GetName() + " " + cred.User() + " " + cred.Group();

    CreatorCred = cred;
    CreatorRoot = container.RootPath;

    /* Set default credentials to creator */
    VolumeOwner = cred;

    /* Apply properties */
    error = ApplyConfig(cfg);
    if (error)
        return error;

    /* Verify storage path */
    if (HaveStorage() && backend != "rbd" && backend != "tmpfs") {
        auto path = GetStorage();
        if (!path.IsAbsolute())
            return TError(EError::InvalidValue, "Storage path must be absolute");
        if (!path.IsNormal())
            return TError(EError::InvalidValue, "Storage path must be normalized");

        /* Convert path to host-based */
        path = container.RootPath / Storage;
        Storage = path.ToString();

        if (!path.Exists())
            return TError(EError::InvalidValue, "Storage path does not exist");
        if (!path.IsDirectoryFollow()) {
            if (path.IsRegularFollow() && (BackendType == "" || BackendType == "loop")) {
                IsFileStorage = true;
            } else
                return TError(EError::InvalidValue, "Storage path must be a directory");
        }
        if (!path.CanWrite(cred))
            return TError(EError::Permission, "Storage path usage not permitted");
    }

    /* Verify default credentials */
    if (VolumeOwner.Uid != cred.Uid && !cred.IsRootUser())
        return TError(EError::Permission, "Changing user is not permitted");

    if (VolumeOwner.Gid != cred.Gid && !cred.IsRootUser() &&
            !cred.IsMemberOf(VolumeOwner.Gid))
        return TError(EError::Permission, "Changing group is not permitted");

    /* Verify and resolve layers */
    for (auto &l: Layers) {
        TPath layer(l);
        if (!layer.IsNormal())
            return TError(EError::InvalidValue, "Layer path must be normalized");
        if (layer.IsAbsolute()) {
            layer = container.RootPath / layer;
            l = layer.ToString();
            if (!layer.Exists())
                return TError(EError::LayerNotFound, "Layer not found");
            /* Racy. Permissions and isolation will be rechecked later */
            if (!layer.CanWrite(cred))
                return TError(EError::Permission, "Layer path not permitted: " + l);
        } else {
            error = ValidateLayerName(l);
            if (error)
                return error;
            layer = Place / config().volumes().layers_dir() / layer;
        }
        if (!layer.Exists())
            return TError(EError::LayerNotFound, "Layer not found");
        if (!layer.IsDirectoryFollow())
            return TError(EError::InvalidValue, "Layer must be a directory");
    }

    /* Verify guarantees */
    if (cfg.count(V_SPACE_LIMIT) && cfg.count(V_SPACE_GUARANTEE) &&
            SpaceLimit < SpaceGuarantee)
        return TError(EError::InvalidValue, "Space guarantree bigger than limit");

    if (cfg.count(V_INODE_LIMIT) && cfg.count(V_INODE_GUARANTEE) &&
            InodeLimit < InodeGuarantee)
        return TError(EError::InvalidValue, "Inode guarantree bigger than limit");

    /* Autodetect volume backend */
    if (!cfg.count(V_BACKEND)) {
        if ((HaveQuota() && !TVolumeNativeBackend::Supported()) || IsFileStorage)
            BackendType = "loop";
        else if (HaveLayers() && TVolumeOverlayBackend::Supported())
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

    error = CheckGuarantee(SpaceGuarantee, InodeGuarantee);
    if (error)
        return error;

    return TError::Success();
}

TError TVolume::Build() {
    auto lock = ScopedLock();
    TPath storage = GetStorage();
    TPath path = Path;
    TPath internal = GetInternal("");

    L_ACT() << "Build volume: " << path
            << " backend: " << BackendType << std::endl;

    TError error = internal.Mkdir(0755);
    if (error)
        goto err_internal;

    if (!HaveStorage()) {
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

    if (HaveLayers() && BackendType != "overlay") {
        L_ACT() << "Merge layers into volume: " << path << std::endl;


        for (auto &name : Layers) {
            if (name[0] == '/') {
                TPath temp;
                TFile pin;

                error = pin.OpenDir(name);
                if (error)
                    goto err_merge;

                if (CreatorRoot.InnerPath(pin.RealPath()).IsEmpty()) {
                    error = TError(EError::Permission, "Layer path outside root: " + name);
                    goto err_merge;
                }

                temp = GetInternal("temp");
                error = temp.Mkdir(0700);
                if (!error)
                    error = temp.BindRemount(pin.ProcPath(), MS_RDONLY | MS_NODEV);
                if (!error)
                    error = temp.Remount(MS_PRIVATE);
                if (error) {
                    (void)temp.Rmdir();
                    goto err_merge;
                }

                pin.Close();

                if (!temp.CanWrite(CreatorCred))
                    error = TError(EError::Permission, "Layer path not permitted: " + name);
                else
                    error = CopyRecursive(temp, path);

                (void)temp.UmountAll();
                (void)temp.Rmdir();
            } else {
                TPath layer = Place / config().volumes().layers_dir() / name;
                error = CopyRecursive(layer, path);
            }
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

    IsReady = true;

    return Save();

err_merge:
err_save:
    (void)Backend->Destroy();
err_build:
    if (IsAutoPath) {
        (void)path.RemoveAll();
    }
err_path:
    if (!HaveStorage())
        (void)storage.RemoveAll();
err_storage:
    (void)internal.RemoveAll();
err_internal:
    return error;
}

TError TVolume::Clear() {
    auto lock = ScopedLock();
    L_ACT() << "Clear volume: " << Path << std::endl;
    return Backend->Clear();
}

TError TVolume::Destroy(void) {
    std::list<std::shared_ptr<TVolume>> plan;
    TError error, ret;

    auto volumes_lock = LockVolumes();

    Volumes.erase(Path);
    plan.push_front(shared_from_this());

    /* Remove sub-volumes */
    for (auto it = Volumes.begin(); it != Volumes.end(); ) {
        if (!Path.InnerPath(it->second->Path).IsEmpty()) {
            plan.push_front(it->second);
            it = Volumes.erase(it);
        } else
            ++it;
    }

    for (auto &volume: plan) {
        volume->IsDying = true;
        for (auto &name: volume->Containers) {
            L_ACT()  << "Forced unlink volume " << volume->Path
                     << " from " << name << std::endl;
            volumes_lock.unlock();
            auto containers_lock = LockContainers();
            auto container = TContainer::Find(name);
            containers_lock.unlock();
            volumes_lock.lock();
            if (container)
                container->Volumes.erase(std::find(container->Volumes.begin(),
                            container->Volumes.end(), volume));
        }
    }

    volumes_lock.unlock();

    for (auto &volume: plan) {
        error = volume->DestroyOne();
        if (error && !ret)
            ret = error;
    }

    return ret;
}

TError TVolume::DestroyOne(void) {
    L_ACT() << "Destroy volume: " << Path
            << " backend: " << BackendType << std::endl;

    auto lock = ScopedLock();

    TPath internal = GetInternal("");
    TPath storage = GetStorage();
    TError ret, error;

    if (Backend) {
        error = Backend->Destroy();
        if (error) {
            L_ERR() << "Can't destroy volume backend: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (!HaveStorage() && storage.Exists()) {
        error = storage.RemoveAll();
        if (error) {
            L_ERR() << "Can't remove storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (IsAutoPath && Path.Exists()) {
        error = Path.RemoveAll();
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

    TPath node(VolumesKV / Id);
    error = node.Unlink();
    if (!ret && error)
        ret = error;

    lock.unlock();

    for (auto &layer: Layers) {
        if (StringStartsWith(layer, "_weak_")) {
            error = RemoveLayer(layer, Place);
            if (error && error.GetError() != EError::Busy)
                L_ERR() << "Cannot remove layer: " << error << std::endl;
        }
    }

    return ret;
}

TError TVolume::StatFS(TStatFS &result) const {
    return Backend->StatFS(result);
}

TError TVolume::Tune(const std::map<std::string, std::string> &properties) {
    TError error;

    for (auto &p : properties) {
        if (p.first != V_INODE_LIMIT &&
            p.first != V_INODE_GUARANTEE &&
            p.first != V_SPACE_LIMIT &&
            p.first != V_SPACE_GUARANTEE)
            /* Prop not found omitted */
                return TError(EError::InvalidProperty,
                              "Volume property " + p.first + " cannot be changed");
    }

    auto lock = ScopedLock();

    if (!IsReady || IsDying)
        return TError(EError::Busy, "Volume not ready");

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

        error = CheckGuarantee(space_guarantee, inode_guarantee);
        if (error)
            return error;

        SpaceGuarantee = space_guarantee;
        InodeGuarantee = inode_guarantee;
    }

    return Save();
}

TError TVolume::Resize(uint64_t space_limit, uint64_t inode_limit) {
    L_ACT() << "Resize volume: " << Path << " to bytes: "
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

TError TVolume::LinkContainer(TContainer &container) {
    auto volumes_lock = LockVolumes();

    auto it = std::find(container.Volumes.begin(),
                        container.Volumes.end(), shared_from_this());
    if (it != container.Volumes.end())
        return TError(EError::VolumeAlreadyLinked, "Already linked");

    if (IsDying)
        return TError(EError::Busy, "Volume is dying");

    Containers.push_back(container.GetName());
    TError error = Save();
    if (error)
        Containers.pop_back();
    else
        container.Volumes.emplace_back(shared_from_this());
    return error;
}

TError TVolume::UnlinkContainer(TContainer &container) {
    auto volumes_lock = LockVolumes();

    auto it = std::find(container.Volumes.begin(),
                        container.Volumes.end(), shared_from_this());
    if (it == container.Volumes.end())
        return TError(EError::VolumeNotLinked, "Container is not linked");

    if (IsDying)
        return TError(EError::Busy, "Volume is dying");

    container.Volumes.erase(it);
    Containers.erase(std::remove(Containers.begin(), Containers.end(), container.GetName()),
                     Containers.end());
    if (Containers.empty())
        IsDying = true;

    return Save();
}

TStringMap TVolume::DumpState(const TPath &root) {
    auto lock = ScopedLock();
    TStringMap ret;
    TStatFS stat;

    if (IsReady && !StatFS(stat)) {
        ret[V_SPACE_USED] = std::to_string(stat.SpaceUsage);
        ret[V_INODE_USED] = std::to_string(stat.InodeUsage);
        ret[V_SPACE_AVAILABLE] = std::to_string(stat.SpaceAvail);
        ret[V_INODE_AVAILABLE] = std::to_string(stat.InodeAvail);
    }

    if (Storage.empty() || BackendType == "rbd")
        ret[V_STORAGE] = Storage;
    else
        ret[V_STORAGE] = root.InnerPath(Storage).ToString();

    ret[V_BACKEND] = BackendType;
    ret[V_USER] = VolumeOwner.User();
    ret[V_GROUP] = VolumeOwner.Group();
    ret[V_PERMISSIONS] = StringFormat("%#o", VolumePerms);
    ret[V_CREATOR] = Creator;
    ret[V_READY] = BoolToString(IsReady && !IsDying);
    ret[V_PRIVATE] = Private;
    ret[V_READ_ONLY] = BoolToString(IsReadOnly);
    ret[V_SPACE_LIMIT] = std::to_string(SpaceLimit);
    ret[V_INODE_LIMIT] = std::to_string(InodeLimit);
    ret[V_SPACE_GUARANTEE] = std::to_string(SpaceGuarantee);
    ret[V_INODE_GUARANTEE] = std::to_string(InodeGuarantee);

    if (HaveLayers()) {
        std::vector<std::string> layers = Layers;

        for (auto &l: layers) {
            TPath path(l);
            if (path.IsAbsolute())
                l = root.InnerPath(path).ToString();
        }
        ret[V_LAYERS] = MergeEscapeStrings(layers, ';');
    }

    if (CustomPlace)
        ret[V_PLACE] = Place.ToString();

    return ret;
}

TError TVolume::Save() {
    TKeyValue node(VolumesKV / Id);
    TError error;
    std::string tmp;

    /*
     * Storing all state values on save,
     * the previous scheme stored knobs selectively.
     */

    node.Set(V_ID, Id);
    node.Set(V_PATH, Path.ToString());
    node.Set(V_AUTO_PATH, BoolToString(IsAutoPath));
    node.Set(V_STORAGE, Storage);
    node.Set(V_BACKEND, BackendType);
    node.Set(V_USER, VolumeOwner.User());
    node.Set(V_GROUP, VolumeOwner.Group());
    node.Set(V_PERMISSIONS, StringFormat("%#o", VolumePerms));
    node.Set(V_CREATOR, Creator);
    node.Set(V_READY, BoolToString(IsReady));
    node.Set(V_PRIVATE, Private);
    node.Set(V_CONTAINERS, MergeEscapeStrings(Containers, ';'));
    node.Set(V_LOOP_DEV, std::to_string(LoopDev));
    node.Set(V_READ_ONLY, BoolToString(IsReadOnly));
    node.Set(V_LAYERS, MergeEscapeStrings(Layers, ';'));
    node.Set(V_SPACE_LIMIT, std::to_string(SpaceLimit));
    node.Set(V_SPACE_GUARANTEE, std::to_string(SpaceGuarantee));
    node.Set(V_INODE_LIMIT, std::to_string(InodeLimit));
    node.Set(V_INODE_GUARANTEE, std::to_string(InodeGuarantee));

    if (CustomPlace)
        node.Set(V_PLACE, Place.ToString());

    return node.Save();
}

TError TVolume::Restore(const TKeyValue &node) {
    if (!node.Has(V_ID))
        return TError(EError::InvalidValue, "No volume id stored");

    Place = config().volumes().default_place();
    CustomPlace = false;

    TError error = ApplyConfig(node.Data);
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

std::vector<TVolumeProperty> VolumeProperties = {
    { V_BACKEND,     "plain|bind|tmpfs|quota|native|overlay|loop|rbd (default - autodetect)", false },
    { V_STORAGE,     "path to data storage (default - internal)", false },
    { V_READY,       "true|false - contruction complete (ro)", true },
    { V_PRIVATE,     "user-defined property", false },
    { V_USER,        "user (default - creator)", false },
    { V_GROUP,       "group (default - creator)", false },
    { V_PERMISSIONS, "directory permissions (default - 0775)", false },
    { V_CREATOR,     "container user group (ro)", true },
    { V_READ_ONLY,   "true|false (default - false)", true },
    { V_LAYERS,      "top-layer;...;bottom-layer - overlayfs layers", false },
    { V_PLACE,       "place for layers and default storage (optional)", false },
    { V_SPACE_LIMIT, "disk space limit (dynamic, default zero - unlimited)", false },
    { V_INODE_LIMIT, "disk inode limit (dynamic, default zero - unlimited)", false },
    { V_SPACE_GUARANTEE,    "disk space guarantee (dynamic, default - zero)", false },
    { V_INODE_GUARANTEE,    "disk inode guarantee (dynamic, default - zero)", false },
    { V_SPACE_USED,  "current disk space usage (ro)", true },
    { V_INODE_USED,  "current disk inode used (ro)", true },
    { V_SPACE_AVAILABLE,    "available disk space (ro)", true },
    { V_INODE_AVAILABLE,    "available disk inodes (ro)", true },
};

TError TVolume::Create(const TPath &path, const TStringMap &cfg,
                       TContainer &container, const TCred &cred,
                       std::shared_ptr<TVolume> &volume) {
    TError error;

    volume = std::make_shared<TVolume>();

    auto volumes_lock = LockVolumes();

    if (!path.IsEmpty() && Volumes.find(path) != Volumes.end())
        return TError(EError::VolumeAlreadyExists, "Volume already exists");

    volume->Id = std::to_string(NextId++);

    error = volume->Configure(path, cfg, container, cred);
    if (error)
        return error;

    Volumes[volume->Path] = volume;

    volumes_lock.unlock();

    error = volume->LinkContainer(container);
    if (error) {
        L_WRN() << "Cannot link volume: " << error << std::endl;
        volume->Destroy();
        return error;
    }

    error = volume->Build();
    if (error) {
        L_WRN() << "Cannot build volume: " << error << std::endl;
        volume->Destroy();
        return error;
    }

    return TError::Success();
}

void TVolume::RestoreAll(void) {
    std::list<TKeyValue> nodes;
    TError error;

    TPath place(config().volumes().default_place());
    error = CheckPlace(place, true);
    if (error)
        L_ERR() << "Cannot prepare place: " << error << std::endl;

    L_ACT() << "Remove stale layers..." << std::endl;
    TPath layers_tmp = place / config().volumes().layers_dir() / "_tmp_";
    error = layers_tmp.ClearDirectory();
    if (error)
        L_ERR() << "Cannot remove stale layers: " << error << std::endl; 

    error = TKeyValue::ListAll(VolumesKV, nodes);
    if (error)
        L_ERR() << "Cannot list nodes: " << error << std::endl;

    for (auto &node : nodes) {
        L_ACT() << "Restore volume: " << node.Path << std::endl;

        error = node.Load();
        if (error) {
            L_WRN() << "Cannot load " << node.Path << " removed: " << error << std::endl;
            node.Path.Unlink();
            continue;
        }

        auto volume = std::make_shared<TVolume>();

        error = volume->Restore(node);
        if (error) {
            L_WRN() << "Corrupted volume " << node.Path << " removed: " << error << std::endl;
            (void)volume->Destroy();
            continue;
        }

        uint64_t id;
        if (!StringToUint64(volume->Id, id)) {
            if (id >= NextId)
                NextId = id + 1;
        }

        Volumes[volume->Path] = volume;

        auto containers_lock = LockContainers();

        for (auto &name: volume->Containers) {
            auto container = TContainer::Find(name);
            if (container)
                container->Volumes.emplace_back(volume);
            else
                L_WRN() << "Cannot find container " << name << std::endl;
        }

        containers_lock.unlock();

        error = volume->Save();
        if (error) {
            (void)volume->Destroy();

            continue;
        }

        L() << "Volume " << volume->Path << " restored" << std::endl;
    }

    TPath volumes = place / config().volumes().volume_dir();

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
}

void TVolume::DestroyAll(void) {
    auto volumes_lock = LockVolumes();
    while (!Volumes.empty()) {
        auto volume = Volumes.rbegin()->second;
        volumes_lock.unlock();
        volume->Destroy();
        volumes_lock.lock();
    }
}

TError TVolume::ApplyConfig(const TStringMap &cfg) {
    TError error;

    for (auto &prop : cfg) {

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
            Storage = prop.second;

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
            error = StringToBool(prop.second, IsReady);
            if (error)
                return error;

        } else if (prop.first == V_PRIVATE) {
            Private = prop.second;

        } else if (prop.first == V_CONTAINERS) {
            SplitEscapedString(prop.second, Containers, ';');

        } else if (prop.first == V_LOOP_DEV) {
            error = StringToInt(prop.second, LoopDev);
            if (error)
                return error;

        } else if (prop.first == V_READ_ONLY) {
            error = StringToBool(prop.second, IsReadOnly);
            if (error)
                return error;

        } else if (prop.first == V_LAYERS) {
            SplitEscapedString(prop.second, Layers, ';');

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

        } else if (prop.first == V_PLACE) {

            Place = prop.second;
            CustomPlace = true;

        } else {
            return TError(EError::InvalidValue, "Invalid value name: " + prop.first);
        }
    }

    return TError::Success();
}
