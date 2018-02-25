#include <sstream>
#include <algorithm>
#include <condition_variable>

#include "volume.hpp"
#include "storage.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/quota.hpp"
#include "config.hpp"
#include "kvalue.hpp"
#include "helpers.hpp"
#include "client.hpp"
#include "filesystem.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <linux/falloc.h>
#include <linux/kdev_t.h>
#include <linux/loop.h>
}

TPath VolumesKV;
std::mutex VolumesMutex;
std::map<TPath, std::shared_ptr<TVolume>> Volumes;
std::map<TPath, std::shared_ptr<TVolumeLink>> VolumeLinks;
static uint64_t NextId = 1;

static std::condition_variable VolumesCv;

/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure() {
    return OK;
}

TError TVolumeBackend::Restore() {
    return OK;
}

TError TVolumeBackend::Resize(uint64_t, uint64_t) {
    return TError(EError::NotSupported, "not implemented");
}

std::string TVolumeBackend::ClaimPlace() {
    return Volume->UserStorage() ? "" : Volume->Place.ToString();
}

/* TVolumeDirBackend - directory */

class TVolumeDirBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (Volume->IsAutoPath)
            return TError(EError::InvalidProperty, "Dir backend requires path");

        if (Volume->HaveQuota())
            return TError(EError::InvalidProperty, "Dir backend doesn't support quota");

        if (Volume->IsReadOnly)
            return TError(EError::InvalidProperty, "Dir backed doesn't support read_only");

        if (Volume->HaveStorage())
            return TError(EError::InvalidProperty, "Dir backed doesn't support storage");

        if (Volume->HaveLayers())
            return TError(EError::InvalidProperty, "Dir backed doesn't support layers");

        /* All data is stored right here */
        Volume->InternalPath = Volume->Path;
        Volume->StoragePath = Volume->Path;
        Volume->KeepStorage = true;

        return OK;
    }

    TError Restore() {

        /* Restore configuration */
        Volume->InternalPath = Volume->Path;
        Volume->StoragePath = Volume->Path;
        Volume->KeepStorage = true;

        return OK;
    }

    TError Build() override {
        return OK;
    }

    std::string ClaimPlace() override {
        return "";
    }

    TError Destroy() override {
        return OK;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->Path.StatFS(result);
    }
};

/* TVolumePlainBackend - bindmount */

class TVolumePlainBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (Volume->HaveQuota())
            return TError(EError::InvalidProperty, "Plain backend have no support of quota");

        return OK;
    }

    TError Build() override {
        return Volume->InternalPath.BindRemount(Volume->StoragePath,
                Volume->GetMountFlags() | MS_SLAVE | MS_SHARED);
    }

    std::string ClaimPlace() override {
        return "";
    }

    TError Destroy() override {
        return Volume->InternalPath.UmountAll();
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolumeBindBackend - bind mount */

class TVolumeBindBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (!Volume->HaveStorage())
            return TError(EError::InvalidProperty, "bind backed require storage");

        if (Volume->HaveQuota())
            return TError(EError::InvalidProperty, "bind backend doesn't support quota");

        if (Volume->HaveLayers())
            return TError(EError::InvalidProperty, "bind backend doesn't support layers");

        return OK;
    }

    TError Build() override {
        return Volume->InternalPath.BindRemount(Volume->StoragePath,
                Volume->GetMountFlags() | MS_SLAVE | MS_SHARED);
    }

    std::string ClaimPlace() override {
        return "";
    }

    TError Destroy() override {
        return Volume->InternalPath.UmountAll();
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolumeRBindBackend - recursive bind mount */

class TVolumeRBindBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (!Volume->HaveStorage())
            return TError(EError::InvalidProperty, "rbind backed require storage");

        if (Volume->HaveQuota())
            return TError(EError::InvalidProperty, "rbind backend doesn't support quota");

        if (Volume->HaveLayers())
            return TError(EError::InvalidProperty, "rbind backend doesn't support layers");

        return OK;
    }

    TError Build() override {
        return Volume->InternalPath.BindRemount(Volume->StoragePath,
                Volume->GetMountFlags() | MS_REC | MS_SLAVE | MS_SHARED);
    }

    std::string ClaimPlace() override {
        return "";
    }

    TError Destroy() override {
        return Volume->InternalPath.UmountAll();
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolumeTmpfsBackend - tmpfs */

class TVolumeTmpfsBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (!Volume->SpaceLimit)
            return TError(EError::InvalidProperty, "tmpfs backend requires space_limit");

        if (Volume->HaveStorage())
            return TError(EError::InvalidProperty, "tmpfs backed doesn't support storage");

        return OK;
    }

    TError Build() override {
        std::vector<std::string> opts;

        if (Volume->SpaceLimit)
            opts.emplace_back("size=" + std::to_string(Volume->SpaceLimit));

        if (Volume->InodeLimit)
            opts.emplace_back("nr_inodes=" + std::to_string(Volume->InodeLimit));

        return Volume->InternalPath.Mount("porto_tmpfs_" + Volume->Id, "tmpfs",
                                          Volume->GetMountFlags(), opts);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        std::vector<std::string> opts;

        if (space_limit)
            opts.emplace_back("size=" + std::to_string(space_limit));

        if (inode_limit)
            opts.emplace_back("nr_inodes=" + std::to_string(inode_limit));

        return Volume->InternalPath.Mount("porto_tmpfs_" + Volume->Id, "tmpfs",
                                          Volume->GetMountFlags() | MS_REMOUNT,
                                          opts);
    }

    std::string ClaimPlace() override {
        return "tmpfs";
    }

    TError Destroy() override {
        return Volume->InternalPath.UmountAll();
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolumeQuotaBackend - project quota */

class TVolumeQuotaBackend : public TVolumeBackend {
public:
    TError Configure() override {

        if (Volume->IsAutoPath)
            return TError(EError::InvalidProperty, "Quota backend requires path");

        if (!Volume->HaveQuota())
            return TError(EError::InvalidProperty, "Quota backend requires space_limit");

        if (Volume->IsReadOnly)
            return TError(EError::InvalidProperty, "Quota backed doesn't support read_only");

        if (Volume->HaveStorage())
            return TError(EError::InvalidProperty, "Quota backed doesn't support storage");

        if (Volume->HaveLayers())
            return TError(EError::InvalidProperty, "Quota backed doesn't support layers");

        /* All data is stored right here */
        Volume->InternalPath = Volume->Path;
        Volume->StoragePath = Volume->Path;
        Volume->KeepStorage = true;

        return OK;
    }

    TError Restore() {

        /* Restore configuration */
        Volume->InternalPath = Volume->Path;
        Volume->StoragePath = Volume->Path;
        Volume->KeepStorage = true;

        return OK;
    }

    TError Build() override {
        TProjectQuota quota(Volume->Path);
        TError error;

        quota.SpaceLimit = Volume->SpaceLimit;
        quota.InodeLimit = Volume->InodeLimit;
        L_ACT("Creating project quota: {} bytes: {} inodes: {}",
              quota.Path, quota.SpaceLimit, quota.InodeLimit);
        return quota.Create();
    }

    std::string ClaimPlace() override {
        return "";
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->Path);
        TError error;

        L_ACT("Destroying project quota: {}", quota.Path);
        return quota.Destroy();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->Path);

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        L_ACT("Resizing project quota: {}", quota.Path);
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        return TProjectQuota(Volume->Path).StatFS(result);
    }
};

/* TVolumeNativeBackend - project quota + bindmount */

class TVolumeNativeBackend : public TVolumeBackend {
public:

    static bool Supported(const TPath &place) {
        static bool printed = false;

        if (!config().volumes().enable_quota())
            return false;

        TProjectQuota quota(place / PORTO_VOLUMES);
        TError error = quota.Enable();
        if (!printed) {
            printed = true;
            if (!error)
                L_SYS("Project quota is supported: {}", quota.Path.c_str());
            else
                L_SYS("Project quota not supported: {} {}", quota.Path.c_str(), error);
        }

        return !error;
    }

    TError Configure() override {

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::NotSupported, "project quota is disabled");

        return OK;
    }

    TError Build() override {
        TProjectQuota quota(Volume->StoragePath);
        TError error;

        if (Volume->HaveQuota()) {
            quota.SpaceLimit = Volume->SpaceLimit;
            quota.InodeLimit = Volume->InodeLimit;
            L_ACT("Creating project quota: {} bytes: {} inodes: {}",
                  quota.Path, quota.SpaceLimit, quota.InodeLimit);
            error = quota.Create();
            if (error)
                return error;
        }

        return Volume->InternalPath.BindRemount(Volume->StoragePath,
                Volume->GetMountFlags() | MS_SLAVE | MS_SHARED);
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->StoragePath);
        TError error = Volume->InternalPath.UmountAll();

        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT("Destroying project quota: {}", quota.Path);
            TError error2 = quota.Destroy();
        }

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->StoragePath);

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT("Creating project quota: {}", quota.Path);
            return quota.Create();
        }
        L_ACT("Resizing project quota: {}", quota.Path);
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        if (Volume->HaveQuota())
            return TProjectQuota(Volume->StoragePath).StatFS(result);
        return Volume->InternalPath.StatFS(result);
    }
};

static TError SetupLoopDev(const TFile &file, const TPath &path, int &loopNr) {
    static std::mutex BigLoopLock;
    TFile ctl, dev;
    struct loop_info64 info;
    int nr, retry = 10;
    TError error;

    error = ctl.OpenReadWrite("/dev/loop-control");
    if (error)
        return error;

    if (config().volumes().direct_io_loop() &&
            fcntl(file.Fd, F_SETFL, fcntl(file.Fd, F_GETFL) | O_DIRECT))
        L_WRN("Cannot enable O_DIRECT for loop {}", TError::System("fcntl"));

    auto lock = std::unique_lock<std::mutex>(BigLoopLock);

again:
    nr = ioctl(ctl.Fd, LOOP_CTL_GET_FREE);
    if (nr < 0)
        return TError::System("ioctl(LOOP_CTL_GET_FREE)");

    error = dev.OpenReadWrite("/dev/loop" + std::to_string(nr));
    if (error)
        return error;

    if (ioctl(dev.Fd, LOOP_SET_FD, file.Fd) < 0) {
        if (errno == EBUSY) {
            if (!ioctl(dev.Fd, LOOP_GET_STATUS64, &info) || errno == ENXIO) {
                if (--retry > 0)
                    goto again;
            }
        }
        return TError::System("ioctl(LOOP_SET_FD)");
    }

    memset(&info, 0, sizeof(info));
    strncpy((char *)info.lo_file_name, path.c_str(), LO_NAME_SIZE - 1);

    if (ioctl(dev.Fd, LOOP_SET_STATUS64, &info) < 0) {
        error = TError::System("ioctl(LOOP_SET_STATUS64)");
        (void)ioctl(dev.Fd, LOOP_CLR_FD, 0);
        return error;
    }

    loopNr = nr;
    return error;
}

TError PutLoopDev(const int loopNr) {
    TFile loop;
    TError error = loop.OpenReadWrite("/dev/loop" + std::to_string(loopNr));
    if (!error && ioctl(loop.Fd, LOOP_CLR_FD, 0) < 0)
        return TError::System("ioctl(LOOP_CLR_FD)");
    return error;
}

/* TVolumeLoopBackend - ext4 image + loop device */

class TVolumeLoopBackend : public TVolumeBackend {
    static constexpr const char *AutoImage = "loop.img";

public:

    static TPath ImagePath(const TPath &storage) {
        if (storage.IsRegularFollow())
            return storage;
        return storage / AutoImage;
    }

    TError Configure() override {
        TPath image = ImagePath(Volume->StoragePath);

        if (!image.Exists() && !Volume->SpaceLimit)
            return TError(EError::InvalidProperty,
                          "loop backend requires space_limit");

        /* Do not allow read-write loop share storage */
        if (Volume->HaveStorage()) {
            for (auto &it: Volumes) {
                auto other = it.second;
                if (other->BackendType != "loop" ||
                        (other->IsReadOnly && Volume->IsReadOnly) ||
                        !image.IsSameInode(ImagePath(other->StoragePath)))
                    continue;
                return TError(EError::Busy, "Storage already used by volume " + other->Path.ToString());
            }
        }

        return OK;
    }

    TPath GetLoopDevice() {
        if (Volume->Device < 0)
            return TPath();
        return TPath("/dev/loop" + std::to_string(Volume->Device));
    }

    static TError MakeImage(TFile &file, const TFile &dir, TPath &path, off_t size, off_t guarantee) {
        TError error;

        L_ACT("Allocate loop image with size {} guarantee {}", size, guarantee);

        if (ftruncate(file.Fd, size))
            return TError::System("truncate(" + path.ToString() + ")");

        if (guarantee && fallocate(file.Fd, FALLOC_FL_KEEP_SIZE, 0, guarantee))
            return TError(EError::ResourceNotAvailable, errno,
                           "cannot fallocate guarantee " + std::to_string(guarantee));

        return RunCommand({ "mkfs.ext4", "-q", "-F", "-m", "0", "-E", "nodiscard",
                            "-O", "^has_journal", path.ToString()}, dir);
    }

    static TError ResizeImage(const TFile &file, const TFile &dir, const TPath &path,
                              off_t current, off_t target) {
        std::string size = std::to_string(target >> 10) + "K";
        TError error;

        if (current < target && ftruncate(file.Fd, target))
            return TError::System("truncate(" + path.ToString() + ")");

        error = RunCommand({"resize2fs", "-f", path.ToString(), size}, dir);

        if (!error && current > target && ftruncate(file.Fd, target))
            error = TError::System("truncate(" + path.ToString() + ")");

        return error;
    }

    static TError ResizeLoopDev(int loopNr, const TPath &image, off_t current, off_t target) {
        auto path = "/dev/loop" + std::to_string(loopNr);
        auto size = std::to_string(target >> 10) + "K";
        TError error;
        TFile dev;

        if (target < current)
            return TError(EError::NotSupported, "Online shrink is not supported yet");

        error = dev.OpenReadWrite(path);
        if (error)
            return error;

        error = image.Truncate(target);
        if (error)
            return error;

        if (ioctl(dev.Fd, LOOP_SET_CAPACITY, 0) < 0)
            return TError::System("ioctl(LOOP_SET_CAPACITY)");

        return RunCommand({"resize2fs", path, size});
    }

    TError Build() override {
        TPath path = ImagePath(Volume->StoragePath);
        struct stat st;
        TError error;
        TFile file;
        bool file_storage = false;

        if (!Volume->StorageFd.Stat(st) && S_ISREG(st.st_mode)) {
            error = file.Dup(Volume->StorageFd);
            file_storage = true;
        } else if (!Volume->StorageFd.StatAt(AutoImage, true, st) && S_ISREG(st.st_mode)) {
            error = file.OpenAt(Volume->StorageFd, AutoImage,
                                (Volume->IsReadOnly ? O_RDONLY : O_RDWR) |
                                O_CLOEXEC | O_NOCTTY, 0);
        } else {
            error = file.OpenAt(Volume->StorageFd, AutoImage,
                                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (!error) {
                Volume->KeepStorage = false; /* New storage */
                error = MakeImage(file, Volume->StorageFd, path, Volume->SpaceLimit, Volume->SpaceGuarantee);
                if (error)
                    Volume->StorageFd.UnlinkAt(AutoImage);
            }
            if (!error)
                error = file.Stat(st);
        }
        if (error)
            return error;

        if (!Volume->SpaceLimit) {
            Volume->SpaceLimit = st.st_size;
        } else if (!Volume->IsReadOnly && (uint64_t)st.st_size != Volume->SpaceLimit) {
            error = ResizeImage(file, file_storage ? TFile() : Volume->StorageFd,
                                path, st.st_size, Volume->SpaceLimit);
            if (error)
                return error;
        }

        error = SetupLoopDev(file, path, Volume->Device);
        if (error)
            return error;

        error = Volume->InternalPath.Mount(GetLoopDevice(), "ext4",
                                           Volume->GetMountFlags(), {});
        if (error) {
            PutLoopDev(Volume->Device);
            Volume->Device = -1;
        }

        return error;
    }

    TError Destroy() override {
        TPath loop = GetLoopDevice();

        if (Volume->Device < 0)
            return OK;

        L_ACT("Destroy loop {}", loop);
        TError error = Volume->InternalPath.UmountAll();
        TError error2 = PutLoopDev(Volume->Device);
        if (!error)
            error = error2;
        Volume->Device = -1;
        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        if (Volume->IsReadOnly)
            return TError(EError::Busy, "Volume is read-only");
        if (Volume->SpaceLimit < (512ul << 20))
            return TError(EError::InvalidProperty, "Refusing to online resize loop volume with initial limit < 512M (kernel bug)");

        (void)inode_limit;
        return ResizeLoopDev(Volume->Device, ImagePath(Volume->StoragePath),
                             Volume->SpaceLimit, space_limit);
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
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
                L_ERR("Unexpected success when testing for overlayfs");
            if (errno == EINVAL)
                supported = true;
            else if (errno != ENODEV)
                L_ERR("Unexpected errno when testing for overlayfs {}", errno);
        }

        return supported;
    }

    TError Configure() override {

        if (!Supported())
            return TError(EError::NotSupported, "overlay not supported");

        if (!Volume->HaveLayers())
            return TError(EError::InvalidProperty, "overlay require layers");

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::InvalidProperty, "project quota is disabled");

        return OK;
    }

    TError Build() override {
        TProjectQuota quota(Volume->StoragePath);
        TError error;
        std::string lower;
        int layer_idx = 0;
        TFile upperFd, workFd;

        if (Volume->HaveQuota()) {
            quota.SpaceLimit = Volume->SpaceLimit;
            quota.InodeLimit = Volume->InodeLimit;
            L_ACT("Creating project quota: {} bytes: {} inodes: {}",
                  quota.Path, quota.SpaceLimit, quota.InodeLimit);
            error = quota.Create();
            if (error)
                  return error;
        }

        for (auto &name: Volume->Layers) {
            TPath path, temp;
            TFile pin;

            if (name[0] == '/') {
                error = pin.OpenDir(name);
                if (error)
                    goto err;
                error = CL->WriteAccess(pin);
                if (error) {
                    error = TError(error, "Layer {}", name);
                    goto err;
                }
                path = pin.ProcPath();
            } else {
                TStorage layer(Volume->Place, PORTO_LAYERS, name);
                /* Imported layers are available for everybody */
                (void)layer.Touch();
                path = layer.Path;
            }

            std::string layer_id = "L" + std::to_string(Volume->Layers.size() - ++layer_idx);
            temp = Volume->GetInternal(layer_id);
            error = temp.Mkdir(700);
            if (!error)
                error = temp.BindRemount(path, MS_RDONLY | MS_NODEV | MS_PRIVATE);
            if (error)
                goto err;

            pin.Close();

            if (layer_idx > 1)
                lower += ":";
            lower += layer_id;
        }

        error = Volume->StorageFd.MkdirAt("upper", 0755);
        if (!error)
            Volume->KeepStorage = false; /* New storage */

        error = upperFd.WalkStrict(Volume->StorageFd, "upper");
        if (error)
            goto err;

        (void)Volume->StorageFd.MkdirAt("work", 0755);

        error = workFd.WalkStrict(Volume->StorageFd, "work");
        if (error)
            goto err;

        error = workFd.ClearDirectory();
        if (error)
            goto err;

        error = Volume->GetInternal("").Chdir();
        if (error)
            goto err;

        error = Volume->InternalPath.Mount("overlay", "overlay",
                                   Volume->GetMountFlags(),
                                   { "lowerdir=" + lower,
                                     "upperdir=" + upperFd.ProcPath().ToString(),
                                     "workdir=" + workFd.ProcPath().ToString() });

        if (error && error.Errno == EINVAL && Volume->Layers.size() >= 500)
            error = TError(EError::InvalidValue, "Too many layers, kernel limits is 499 plus 1 for upper");

        (void)TPath("/").Chdir();

err:
        while (layer_idx--) {
            TPath temp = Volume->GetInternal("L" + std::to_string(layer_idx));
            (void)temp.UmountAll();
            (void)temp.Rmdir();
        }

        if (!error)
            return error;

        if (Volume->HaveQuota())
            (void)quota.Destroy();
        return error;
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->StoragePath);
        TError error = Volume->InternalPath.UmountAll();

        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT("Destroying project quota: {}", quota.Path);
            TError error2 = quota.Destroy();
            if (!error)
                error = error2;
        }

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->StoragePath);

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT("Creating project quota: {}", quota.Path);
            return quota.Create();
        }
        L_ACT("Resizing project quota: {}", quota.Path);
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        if (Volume->HaveQuota())
            return TProjectQuota(Volume->StoragePath).StatFS(result);
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolumeSquashBackend - loop + squashfs + overlayfs + quota */

class TVolumeSquashBackend : public TVolumeBackend {
public:

    TError Configure() override {
        if (!TVolumeOverlayBackend::Supported())
            return TError(EError::NotSupported, "overlay not supported");

        if (!Volume->HaveLayers())
            return TError(EError::InvalidProperty, "Backend squash requires image");

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::InvalidProperty, "project quota is disabled");

        return OK;
    }

    TError Build() override {
        TProjectQuota quota(Volume->StoragePath);
        TFile lowerFd, upperFd, workFd;
        std::string lowerdir;
        int layer_idx = 0;
        TError error;
        TPath lower;

        lower = Volume->GetInternal("lower");
        error = lower.Mkdir(0755);
        if (error)
            return error;

        error = lowerFd.OpenRead(Volume->Layers[0]);
        if (error)
            return error;

        error = Volume->StorageFd.MkdirAt("upper", 0755);
        if (!error)
            Volume->KeepStorage = false; /* New storage */

        error = upperFd.WalkStrict(Volume->StorageFd, "upper");
        if (error)
            return error;

        (void)Volume->StorageFd.MkdirAt("work", 0755);

        error = workFd.WalkStrict(Volume->StorageFd, "work");
        if (error)
            return error;

        error = workFd.ClearDirectory();
        if (error)
            return error;

        error = SetupLoopDev(lowerFd, Volume->Layers[0], Volume->Device);
        if (error)
            return error;

        error = lower.Mount("/dev/loop" + std::to_string(Volume->Device),
                            "squashfs", MS_RDONLY | MS_NODEV | MS_NOSUID, {});
        if (error)
            goto err;

        /* shortcut for read-only volumes without extra layers */
        if (Volume->IsReadOnly && Volume->Layers.size() == 1) {
            error = Volume->InternalPath.BindRemount(lower, Volume->GetMountFlags());
            if (error)
                goto err;
            return OK;
        }

        if (Volume->HaveQuota()) {
            quota.SpaceLimit = Volume->SpaceLimit;
            quota.InodeLimit = Volume->InodeLimit;
            L_ACT("Creating project quota: {} bytes: {} inodes: {}",
                  quota.Path, quota.SpaceLimit, quota.InodeLimit);
            error = quota.Create();
            if (error)
                  goto err;
        }

        error = Volume->GetInternal("").Chdir();
        if (error)
            goto err;

        lowerdir = lower.ToString();

        for (auto &name: Volume->Layers) {
            TPath path, temp;
            TFile pin;

            if (name == Volume->Layers[0])
                continue;

            if (name[0] == '/') {
                error = pin.OpenDir(name);
                if (error)
                    goto err;
                error = CL->WriteAccess(pin);
                if (error) {
                    error = TError(error, "Layer {}", name);
                    goto err;
                }
                path = pin.ProcPath();
            } else {
                TStorage layer(Volume->Place, PORTO_LAYERS, name);
                /* Imported layers are available for everybody */
                (void)layer.Touch();
                path = layer.Path;
            }

            std::string layer_id = "L" + std::to_string(Volume->Layers.size() -
                                                        ++layer_idx - 1);
            temp = Volume->GetInternal(layer_id);
            error = temp.Mkdir(700);
            if (!error)
                error = temp.BindRemount(path, MS_RDONLY | MS_NODEV | MS_PRIVATE);
            if (error)
                goto err;

            pin.Close();
            lowerdir += ":" + layer_id;
        }

        error = Volume->InternalPath.Mount("overlay", "overlay",
                                   Volume->GetMountFlags(),
                                   { "lowerdir=" + lowerdir,
                                     "upperdir=" + upperFd.ProcPath().ToString(),
                                     "workdir=" + workFd.ProcPath().ToString() });

        if (error && error.Errno == EINVAL && Volume->Layers.size() >= 500)
            error = TError(EError::InvalidValue, "Too many layers, kernel limits is 499 plus 1 for upper");

        while (layer_idx--) {
            TPath temp = Volume->GetInternal("L" + std::to_string(layer_idx));
            (void)temp.UmountAll();
            (void)temp.Rmdir();
        }

err:
        (void)TPath("/").Chdir();

        if (error) {
            if (Volume->HaveQuota())
                (void)quota.Destroy();
            if (Volume->Device >= 0) {
                lower.UmountAll();
                PutLoopDev(Volume->Device);
                Volume->Device = -1;
            }
        }

        return error;
    }

    TError Destroy() override {
        if (Volume->Device >= 0) {
            Volume->InternalPath.UmountAll();
            Volume->GetInternal("lower").UmountAll();
            PutLoopDev(Volume->Device);
            Volume->Device = -1;
        }

        TProjectQuota quota(Volume->StoragePath);
        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT("Destroying project quota: {}", quota.Path);
            (void)quota.Destroy();
        }

        return OK;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->StoragePath);

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT("Creating project quota: {}", quota.Path);
            return quota.Create();
        }
        L_ACT("Resizing project quota: {}", quota.Path);
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolumeRbdBackend - ext4 in ceph rados block device */

class TVolumeRbdBackend : public TVolumeBackend {
public:

    std::string GetDevice() {
        if (Volume->Device < 0)
            return "";
        return "/dev/rbd" + std::to_string(Volume->Device);
    }

    TError MapDevice(std::string id, std::string pool, std::string image,
                     std::string &device) {
        L_ACT("Map rbd device {}@{}/{}", id, pool, image);
        TError error;
        TFile out;

        error = out.CreateUnnamed("/tmp");
        if (error)
            return error;
        error = RunCommand({"rbd", "--id=" + id, "--pool=" + pool, "map", image}, TFile(), TFile(), out);
        if (error)
            return error;
        error = out.ReadAll(device, 1024);
        if (error)
            return error;
        device = StringTrim(device);
        return OK;
    }

    TError UnmapDevice(std::string device) {
        L_ACT("Unmap rbd device {}", device);
        return RunCommand({"rbd", "unmap", device});
    }

    TError Build() override {
        std::string id, pool, image, device;
        TError error, error2;

        auto tok = SplitEscapedString(Volume->Storage, '@');
        if (tok.size() != 2)
            return TError(EError::InvalidValue, "Invalid rbd storage");
        id = tok[0];
        image = tok[1];
        tok = SplitEscapedString(image, '/');
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

        error = StringToInt(device.substr(8), Volume->Device);
        if (error) {
            UnmapDevice(device);
            return error;
        }

        error = Volume->InternalPath.Mount(device, "ext4",
                                           Volume->GetMountFlags(), {});
        if (error)
            UnmapDevice(device);
        return error;
    }

    TError Destroy() override {
        std::string device = GetDevice();
        TError error, error2;

        if (Volume->Device < 0)
            return OK;

        error = Volume->InternalPath.UmountAll();
        error2 = UnmapDevice(device);
        if (!error)
            error = error2;
        Volume->Device = -1;
        return error;
    }

    TError Resize(uint64_t, uint64_t) override {
        return TError(EError::NotSupported, "rbd backend doesn't suppport resize");
    }

    std::string ClaimPlace() override {
        return "rbd";
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};


/* TVolumeLvmBackend - ext4 on LVM */

class TVolumeLvmBackend : public TVolumeBackend {
public:

    bool Persistent;
    std::string Group;
    std::string Name;
    std::string Thin;
    std::string Origin;
    std::string Device;

    static TError CheckName(const std::string &name) {
        auto pos = name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_.-");
        if (pos != std::string::npos)
            return TError(EError::InvalidValue, "lvm character {:#x} in name", name[pos]);
        return OK;
    }

    TError Configure() override {
        TError error;

        // storage=[Group][/Name][@Thin][:Origin]

        auto col = Volume->Storage.find(':');
        if (col != std::string::npos)
            Origin = Volume->Storage.substr(col + 1);

        auto at = Volume->Storage.find('@');
        if (at != std::string::npos && at < col)
            Thin = Volume->Storage.substr(at + 1, col - at - 1);
        else
            at = col;

        auto sep = Volume->Storage.find('/');
        if (sep != std::string::npos && sep < at)
            Name = Volume->Storage.substr(sep + 1, at - sep - 1);
        else
            sep = at;

        Group = Volume->Storage.substr(0, sep);

        if (Group.empty())
            Group = config().volumes().default_lvm_group();

        Persistent = !Name.empty();
        if (!Persistent)
            Name = "porto_lvm_" + Volume->Id;

        error = CheckName(Group);
        if (error)
            return error;

        error = CheckName(Name);
        if (error)
            return error;

        error = CheckName(Thin);
        if (error)
            return error;

        error = CheckName(Origin);
        if (error)
            return error;

        Device = "/dev/" + Group + "/" + Name;

        if (!Volume->SpaceLimit && !Persistent && Origin.empty())
            return TError(EError::InvalidValue, "lvm space_limit not set");

        if (Group.empty())
            return TError(EError::InvalidValue, "lvm volume group not set");

        if (Persistent && StringStartsWith(Name, "porto_"))
            return TError(EError::InvalidValue, "reserved lvm volume name");

        if (StringStartsWith(Origin, "porto_"))
            return TError(EError::InvalidValue, "origin is temporary volume");

        return OK;
    }

    TError Restore() override {
        return Configure();
    }

    TError Build() override {
        TError error;

        if (!TPath(Device).Exists() || !Persistent) {
            Volume->KeepStorage = false; /* Do chown and chmod */

            if (Origin.size()) {
                error = RunCommand({"lvm", "lvcreate", "--name", Name,
                                    "--snapshot", Group + "/" + Origin,
                                    "--setactivationskip", "n"});
                if (!error && Volume->SpaceLimit)
                    error = Resize(Volume->SpaceLimit, Volume->InodeLimit);
            } else if (Thin.size()) {
                error = RunCommand({"lvm", "lvcreate", "--name", Name, "--thin",
                                    "--virtualsize", std::to_string(Volume->SpaceLimit) + "B",
                                    Group + "/" + Thin});
            } else {
                error = RunCommand({"lvm", "lvcreate", "--name", Name,
                                    "--size", std::to_string(Volume->SpaceLimit) + "B",
                                    Group});
            }
            if (error)
                return error;

            if (Origin.empty()) {
                error = RunCommand({ "mkfs.ext4", "-q", "-m", "0",
                                     "-O", std::string(Persistent ? "" : "^") + "has_journal",
                                     Device});
                if (error)
                    Persistent = false;
            }
        }

        if (!error)
            error = Volume->InternalPath.Mount(Device, "ext4",
                                               Volume->GetMountFlags(),
                                               { Persistent ? "barrier" : "nobarrier",
                                                 "errors=continue" });

        if (error && !Persistent)
            (void)RunCommand({"lvm", "lvremove", "--force", Device });

        return error;
    }

    TError Destroy() override {
        TError error = Volume->InternalPath.UmountAll();
        if (!Persistent) {
            TError error2 = RunCommand({"lvm", "lvremove", "--force", Device});
            if (!error)
                error = error2;
        }
        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t) override {
        return RunCommand({"lvm", "lvresize", "--force", "--resizefs",
                           "--size", std::to_string(space_limit) + "B",
                           Device});
    }

    std::string ClaimPlace() override {
        return "lvm " + Group;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
    }
};

/* TVolume */

std::shared_ptr<TVolumeLink> TVolume::ResolveLinkLocked(const TPath &path) {
    auto it = VolumeLinks.find(path);
    if (it != VolumeLinks.end())
        return it->second;
    return nullptr;
}

std::shared_ptr<TVolumeLink> TVolume::ResolveLink(const TPath &path) {
    auto volumes_lock = LockVolumes();
    return ResolveLinkLocked(path);
}

std::shared_ptr<TVolumeLink> TVolume::ResolveOriginLocked(const TPath &path) {
    if (path.IsAbsolute()) {
        for (auto p = path.NormalPath(); !p.IsRoot(); p = p.DirNameNormal()) {
            auto link = ResolveLinkLocked(p);
            if (link)
                return link;
        }
    }
    return nullptr;
}

std::shared_ptr<TVolumeLink> TVolume::ResolveOrigin(const TPath &path) {
    auto volumes_lock = LockVolumes();
    return ResolveOriginLocked(path);
}

TPath TVolume::ComposePath(const TContainer &ct) const {
    auto volumes_lock = LockVolumes();

    /* prefer own link */
    for (auto &link: ct.VolumeLinks) {
        if (link->Volume.get() == this && link->Target)
            return link->Target;
    }

    /* any reachable path */
    for (auto &link: Links) {
        if (link->HostTarget) {
            TPath path = ct.RootPath.InnerPath(link->HostTarget);
            if (path)
                return path;
        }
    }

    /* volume path */
    return ct.RootPath.InnerPath(Path);
}

std::string TVolume::StateName(EVolumeState state) {
    switch (state) {
    case EVolumeState::Initial:
        return "initial";
    case EVolumeState::Building:
        return "building";
    case EVolumeState::Ready:
        return "ready";
    case EVolumeState::Unlinked:
        return "unlinked";
    case EVolumeState::ToDestroy:
        return "to-destroy";
    case EVolumeState::Destroying:
        return "destroying";
    case EVolumeState::Destroyed:
        return "destroyed";
    default:
        return "unknown";
    }
}

void TVolume::SetState(EVolumeState state) {
    L("Change volume {} state {} -> {}", Path, StateName(State), StateName(state));
    State = state;
    if (state == EVolumeState::Ready || state == EVolumeState::Destroyed)
        VolumesCv.notify_all();
}

TError TVolume::OpenBackend() {
    if (BackendType == "dir")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeDirBackend());
    else if (BackendType == "plain")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumePlainBackend());
    else if (BackendType == "bind")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeBindBackend());
    else if (BackendType == "rbind")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeRBindBackend());
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
    else if (BackendType == "squash")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeSquashBackend());
    else if (BackendType == "lvm")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeLvmBackend());
    else if (BackendType == "rbd")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeRbdBackend());
    else
        return TError(EError::NotSupported, "Unknown volume backend: " + BackendType);

    Backend->Volume = this;

    return OK;
}

/* /place/porto_volumes/<id>/<type> */
TPath TVolume::GetInternal(const std::string &type) const {

    TPath base = Place / PORTO_VOLUMES / Id;
    if (type.size())
        return base / type;
    else
        return base;
}

unsigned long TVolume::GetMountFlags(void) const {
    unsigned flags = 0;

    if (IsReadOnly)
        flags |= MS_RDONLY;

    flags |= MS_NODEV | MS_NOSUID;

    return flags;
}

TError TVolume::CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee) {
    TStatFS current, total;
    TPath storage;

    if (RemoteStorage() || (!space_guarantee && !inode_guarantee))
        return OK;

    if (UserStorage())
        storage = StoragePath;
    else if (HaveStorage())
        storage = Place / PORTO_STORAGE;
    else
        storage = Place / PORTO_VOLUMES;

    TError error = storage.StatFS(total);
    if (error)
        return error;

    StatFS(current);

    /* Check available space as is */
    if (total.SpaceAvail + current.SpaceUsage < space_guarantee)
        return TError(EError::NoSpace, "Not enough space for volume guarantee: " +
                      std::to_string(total.SpaceAvail) + " available " +
                      std::to_string(current.SpaceUsage) + " used");

    if (total.InodeAvail + current.InodeUsage < inode_guarantee &&
            BackendType != "loop")
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used");

    /* Estimate unclaimed guarantees */
    uint64_t space_claimed = 0, space_guaranteed = 0;
    uint64_t inode_claimed = 0, inode_guaranteed = 0;
    for (auto &it: Volumes) {
        auto volume = it.second;
        if (volume.get() == this ||
                volume->StoragePath.GetDev() != storage.GetDev())
            continue;

        /* data stored remotely, plain cannot provide usage */
        if (volume->RemoteStorage() || volume->BackendType == "plain")
            continue;

        TStatFS stat;
        uint64_t volume_space_guarantee = SpaceGuarantee;
        uint64_t volume_inode_guarantee = InodeGuarantee;

        if (!volume_space_guarantee && !volume_inode_guarantee)
            continue;

        volume->StatFS(stat);

        space_guaranteed += volume_space_guarantee;
        if (stat.SpaceUsage < volume_space_guarantee)
            space_claimed += stat.SpaceUsage;
        else
            space_claimed += volume_space_guarantee;

        if (volume->BackendType != "loop") {
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

    if (BackendType != "loop" &&
            total.InodeAvail + current.InodeUsage + inode_claimed <
            inode_guarantee + inode_guaranteed)
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used " +
                      std::to_string(inode_claimed) + " claimed " +
                      std::to_string(inode_guaranteed) + " guaranteed");

    return OK;
}

TError TVolume::ClaimPlace(uint64_t size) {

    if (!VolumeOwnerContainer || !Backend)
        return TError(EError::Busy, "Volume has no backend or owner");

    auto place = Backend->ClaimPlace();
    if (place == "")
        return OK;

    auto lock = LockVolumes();

    if ((!size || size > ClaimedSpace) && !CL->IsInternalUser() &&
            State != EVolumeState::Destroying) {
        for (auto ct = VolumeOwnerContainer; ct; ct = ct->Parent) {
            uint64_t total_limit = ct->PlaceLimit.count("total") ?
                                   ct->PlaceLimit.at("total") : UINT64_MAX;
            uint64_t place_limit = ct->PlaceLimit.count(place) ?
                                   ct->PlaceLimit.at(place) :
                                   ct->PlaceLimit.count("default") ?
                                   ct->PlaceLimit.at("default") : UINT64_MAX;
            uint64_t total_usage = ct->PlaceUsage.count("total") ?
                                   ct->PlaceUsage.at("total") : 0;
            uint64_t place_usage = ct->PlaceUsage.count(place) ?
                                   ct->PlaceUsage.at(place) : 0;

            if ((total_limit != UINT64_MAX && size == 0) ||
                    (place_limit != UINT64_MAX && size == 0) ||
                    (total_usage - ClaimedSpace > UINT64_MAX - size) ||
                    (total_limit < total_usage - ClaimedSpace + size) ||
                    (place_usage - ClaimedSpace > UINT64_MAX - size) ||
                    (place_limit < place_usage - ClaimedSpace + size))
                return TError(EError::ResourceNotAvailable, "Not enough place limit in " + ct->Name);
        }
    }

    for (auto ct = VolumeOwnerContainer; ct; ct = ct->Parent) {
        ct->PlaceUsage["total"] += size - ClaimedSpace;
        ct->PlaceUsage[place] += size - ClaimedSpace;
    }

    ClaimedSpace = size;

    return OK;
}

TError TVolume::DependsOn(const TPath &path) {
    if (State == EVolumeState::Ready && !path.Exists())
        return TError("Required path {} not found", path);

    auto link = ResolveOriginLocked(path);
    if (link) {
        if (link->Volume->State != EVolumeState::Ready)
            return TError(EError::VolumeNotReady, "Required volume {} not ready", link->Volume->Path);
        L("Volume {} depends on volume {}", Path, link->Volume->Path);
        link->Volume->Nested.insert(shared_from_this());
    }

    return OK;
}

TError TVolume::CheckDependencies() {
    TError error;

    if (!IsAutoPath)
        error = DependsOn(Path.DirName());

    if (!error)
        error = DependsOn(Place);

    if (!error && !RemoteStorage())
        error = DependsOn(StoragePath);

    for (auto &l : Layers) {
        TPath layer(l);
        if (!layer.IsAbsolute())
            layer = Place / PORTO_LAYERS / l;
        if (!error)
            error = DependsOn(layer);
    }

    if (error) {
        /* undo dependencies */
        for (auto &it : Volumes)
            it.second->Nested.erase(shared_from_this());
    }

    return error;
}

TError TVolume::CheckConflicts(const TPath &path) {
    if (IsSystemPath(path))
        return TError(EError::Busy, "Path in system directory");

    for (auto &it : Volumes) {
        auto &vol = it.second;

        if (vol->Path == path)
            return TError(EError::Busy, "Path is used by volume {}", vol->Path);

        if (vol->Path.IsInside(path))
            return TError(EError::Busy, "Path overlaps with volume {}", vol->Path);

        if (path.IsInside(vol->Path) && vol->State != EVolumeState::Ready)
            return TError(EError::VolumeNotReady, "Path inside volume {} and it's not ready", vol->Path);

        if (vol->Place.IsInside(path))
            return TError(EError::Busy, "Path overlaps with place {}", vol->Place);

        if (!vol->RemoteStorage() && vol->BackendType != "bind" && vol->BackendType != "rbind" &&
                (vol->StoragePath.IsInside(path) || path.IsInside(vol->StoragePath)))
            return TError(EError::Busy, "Path overlaps with storage {}", vol->StoragePath);

        for (auto &l: vol->Layers) {
            TPath layer(l);
            if (layer.IsAbsolute() && (layer.IsInside(path) || path.IsInside(layer)))
                return TError(EError::Busy, "Path overlaps with layer {}", layer);
        }

        for (auto &link: vol->Links) {
            if (link->HostTarget == path)
                return TError(EError::Busy, "Path is used by volume {} link {} for {}", vol->Path, link->HostTarget, link->Container->Name);
            if (link->HostTarget.IsInside(path))
                return TError(EError::Busy, "Path overlaps with volume {} link {} for {}", vol->Path, link->HostTarget, link->Container->Name);
        }
    }

    return OK;
}

TError TVolume::Configure(const TPath &target_root, const TStringMap &cfg) {
    TError error;

    /* Verify properties */
    for (auto &it: cfg) {
        if (it.first == V_PATH)
            continue;
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

    /* Default user:group */
    VolumeOwner = CL->Cred;
    Creator = CL->ClientContainer->Name + " " + CL->Cred.User() + " " + CL->Cred.Group();

    /* Apply properties */
    error = ApplyConfig(cfg);
    if (error)
        return error;

    /* Verify credentials */
    error = CL->CanControl(VolumeOwner);
    if (error)
        return TError(error, "Volume {}", Path);

    if (VolumeOwner.Gid != CL->Cred.Gid && !CL->IsSuperUser() &&
            !CL->Cred.IsMemberOf(VolumeOwner.Gid))
        return TError(EError::Permission, "Changing owner group is not permitted");

    /* Autodetect volume backend, prefer native or overlay */
    if (BackendType == "") {
        if ((HaveQuota() && !TVolumeNativeBackend::Supported(Place)) ||
                StoragePath.IsRegularFollow())
            BackendType = "loop";
        else if (HaveLayers() && TVolumeOverlayBackend::Supported())
            BackendType = "overlay";
        else if (TVolumeNativeBackend::Supported(Place))
            BackendType = "native";
        else
            BackendType = "plain";
    }

    InternalPath = Place / PORTO_VOLUMES / Id / "volume";

    if (Path) {
        Path = target_root / Path;
    } else {
        if (target_root.IsRoot()) {
            /* /place/porto_volumes/<id>/volume */
            Path = InternalPath;
        } else {
            /* /chroot/porto/volume_<id> */
            TPath porto_path = target_root / PORTO_CHROOT_VOLUMES;
            if (!porto_path.Exists()) {
                error = porto_path.Mkdir(0755);
                if (error)
                    return error;
            }
            Path = porto_path / "volume_" + Id;
        }
        IsAutoPath = true;
    }

    /* Verify storage */
    if (RemoteStorage()) {
        /* They use storage for own purpose */
    } else if (UserStorage()) {
        if (!StoragePath.IsNormal())
            return TError(EError::InvalidValue, "Storage path must be normalized");
        StoragePath = CL->ResolvePath(StoragePath);
        if (!StoragePath.Exists())
            return TError(EError::InvalidValue, "Storage path does not exist");
        if (IsSystemPath(StoragePath))
            return TError(EError::InvalidValue, "Storage in system directory");
        Storage = StoragePath.ToString();
        KeepStorage = true;
    } else if (!HaveStorage()) {
        StoragePath = GetInternal(BackendType);
        KeepStorage = false;
    } else if (StoragePath.IsSimple()) {
        error = TStorage::CheckName(Storage);
        if (error)
            return error;
        TStorage storage(Place, PORTO_STORAGE, Storage);
        StoragePath = storage.Path;
        KeepStorage = storage.Exists();
    } else
        return TError(EError::InvalidValue, "Invalid storage format: " + Storage);

    if (!RemoteStorage()) {
        for (auto &it: Volumes) {
            auto other = it.second;
            if (!other->RemoteStorage() &&
                    StoragePath == other->StoragePath &&
                    Place == other->Place &&
                    (!IsReadOnly || !other->IsReadOnly) &&
                    (!(BackendType == "bind" || BackendType == "rbind") ||
                     !(other->BackendType == "bind" || other->BackendType == "rbind")))
                return TError(EError::Busy, "Storage already in use by volume " +
                        other->Path.ToString());
        }
    }

    /* Verify and resolve layers */
    for (auto &l: Layers) {
        TPath layer(l);
        if (!layer.IsNormal())
            return TError(EError::InvalidValue, "Layer path must be normalized");
        if (layer.IsAbsolute()) {
            layer = CL->ResolvePath(layer);
            l = layer.ToString();
        } else {
            error = TStorage::CheckName(l);
            if (error)
                return error;
            layer = Place / PORTO_LAYERS / layer;
        }
        if (!layer.Exists())
            return TError(EError::LayerNotFound, "Layer not found " + layer.ToString());
        if (!layer.IsDirectoryFollow() && BackendType != "squash")
            return TError(EError::InvalidValue, "Layer must be a directory");
        /* Permissions will be cheked during build */
    }

    if (HaveLayers() && BackendType != "overlay" && BackendType != "squash") {
        if (IsReadOnly)
            return TError(EError::InvalidValue, "Cannot copy layers to read-only volume");
    }

    /* Verify guarantees */
    if (cfg.count(V_SPACE_LIMIT) && cfg.count(V_SPACE_GUARANTEE) &&
            SpaceLimit < SpaceGuarantee)
        return TError(EError::InvalidValue, "Space guarantree bigger than limit");

    if (cfg.count(V_INODE_LIMIT) && cfg.count(V_INODE_GUARANTEE) &&
            InodeLimit < InodeGuarantee)
        return TError(EError::InvalidValue, "Inode guarantree bigger than limit");

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Configure();
    if (error)
        return error;

    error = CheckGuarantee(SpaceGuarantee, InodeGuarantee);
    if (error)
        return error;

    return OK;
}

TError TVolume::Build() {
    L_ACT("Build volume: {} backend: {}", Path, BackendType);

    TError error = GetInternal("").Mkdir(0755);
    if (error)
        return error;

    /* Create and pin storage */
    if (!UserStorage() && !RemoteStorage() && !StoragePath.Exists()) {
        error = StoragePath.Mkdir(0755);
        if (error)
            return error;
    }

    if (FileStorage() && UserStorage() && StoragePath.IsRegularFollow()) {
        if (IsReadOnly)
            error = StorageFd.OpenRead(StoragePath);
        else
            error = StorageFd.OpenReadWrite(StoragePath);
    } else if (!RemoteStorage())
        error = StorageFd.OpenDir(StoragePath);
    if (error)
        return error;

    if (RemoteStorage()) {
        /* Nothing to check */
    } else if (UserStorage()) {
        if (IsReadOnly)
            error = CL->ReadAccess(StorageFd);
        else
            error = CL->WriteAccess(StorageFd);
        if (error)
            return TError(error, "Volume {}", Path);
    } else if (HaveStorage()) {
        TStorage storage(Place, PORTO_STORAGE, Storage);
        error = storage.Load();
        if (error)
            return error;
        error = CL->CanControl(storage.Owner);
        if (error)
            return TError(error, "Storage {}", Storage);
        if (storage.Owner.IsUnknown()) {
            error = storage.SetOwner(VolumeOwner);
            if (error)
                return error;
        }
        if (Private.empty())
            Private = storage.Private;
        else
            error = storage.SetPrivate(Private);
        if (error)
            return error;
        error = storage.Touch();
        if (error)
            return error;
    }

    if (BackendType != "dir" && BackendType != "quota") {
        error = InternalPath.Mkdir(0755);
        if (error)
            return error;
    }

    /* Save volume state before building */
    error = Save();
    if (error)
        return error;

    error = Backend->Build();
    if (error)
        return error;

    if (HaveLayers() && BackendType != "overlay" && BackendType != "squash") {

        L_ACT("Merge layers into volume: {}", Path);

        for (auto &name : Layers) {
            if (name[0] == '/') {
                TPath temp;
                TFile pin;

                error = pin.OpenDir(name);
                if (error)
                    return error;

                error = CL->WriteAccess(pin);
                if (error)
                    return TError(error, "Layer {}", name);

                temp = GetInternal("temp");
                error = temp.Mkdir(0700);
                if (!error)
                    error = temp.BindRemount(pin.ProcPath(), MS_RDONLY | MS_NODEV | MS_PRIVATE);
                if (error) {
                    (void)temp.Rmdir();
                    return error;
                }

                error = CopyRecursive(temp, InternalPath);

                (void)temp.UmountAll();
                (void)temp.Rmdir();
            } else {
                TStorage layer(Place, PORTO_LAYERS, name);
                (void)layer.Touch();
                /* Imported layers are available for everybody */
                error = CopyRecursive(layer.Path, InternalPath);
            }
            if (error)
                return error;
        }

        error = TStorage::SanitizeLayer(InternalPath, true);
        if (error)
            return error;
    }

    /* Initialize cred and perms but do not change is user havn't asked */
    if (!IsReadOnly) {
        if (!KeepStorage || !VolumeCred.IsUnknown()) {
            TCred cred = VolumeCred;
            if (cred.Uid == NoUser)
                cred.Uid = CL->TaskCred.Uid;
            if (cred.Gid == NoGroup)
                cred.Gid = CL->TaskCred.Gid;
            error = InternalPath.Chown(cred);
            if (error)
                return error;
        }
        if (!KeepStorage || VolumePerms) {
            error = InternalPath.Chmod(VolumePerms ?: 0775);
            if (error)
                return error;
        }
    }

    /* Make sure than we saved this before publishing */
    error = Save();
    if (error)
        return error;

    StorageFd.Close();

    /* Keep storage only after successful build */
    if (!KeepStorage && HaveStorage())
        KeepStorage = true;

    BuildTime = FormatTime(time(nullptr));

    return OK;
}

TError TVolume::MountLink(std::shared_ptr<TVolumeLink> link) {
    TError error, error2;

    if (!link->Target)
        return OK;

    auto lock = LockState();
    if (State != EVolumeState::Ready)
        return TError(EError::VolumeNotReady, "Volume {} not ready", Path);

    auto volumes_lock = LockVolumes();
    if (link->Volume.get() != this)
        return TError(EError::InvalidValue, "Wrong volume link");
    TPath host_target = link->Container->RootPath / link->Target;
    link->HostTarget = "";
    auto target_link = ResolveOriginLocked(host_target.DirNameNormal());
    if (host_target != link->Volume->Path) {
        error = TVolume::CheckConflicts(host_target);
        if (error)
            return error;
        VolumeLinks[host_target] = link;
        Statistics->VolumeLinksMounted++;
    }
    link->HostTarget = host_target;
    volumes_lock.unlock();

    L_ACT("Mount volume {} link {} for CT{}:{}", Path, host_target, link->Container->Id, link->Container->Name);

    TFile target_file;
    TPath bound, link_mount;
    unsigned long flags = 0;

    if (target_link && !CL->CanControl(target_link->Volume->VolumeOwner)) {
        bound = target_link->HostTarget;
    } else {
        bound = link->Container->RootPath;
        if (bound.IsRoot())
            bound = "";
    }

    error = target_file.CreatePath(host_target, CL->Cred, bound);
    if (error)
        goto undo;

    if (IsReadOnly || link->ReadOnly)
        flags |= MS_RDONLY;

    if (BackendType == "rbind" || BackendType == "dir")
        flags |= MS_REC;

    /* save state before changes */
    error = Save();
    if (error)
        goto undo;

    link_mount = GetInternal("volume_link");
    (void)link_mount.Mkdir(700);

    /* make private - cannot move from shared mount */
    error = link_mount.BindRemount(link_mount, MS_PRIVATE);
    if (error)
        goto undo;

    /* Start new shared group and make read-only - that isn't propagated */
    error = link_mount.BindRemount(InternalPath, flags | MS_SLAVE | MS_SHARED);
    if (error)
        goto undo;

    /* Move to target path and propagate into namespaces */
    error = link_mount.MoveMount(target_file.ProcPath());
    if (error)
        goto undo;

    link_mount.UmountAll();
    link_mount.Rmdir();

    return OK;

undo:
    link_mount.UmountAll();
    link_mount.Rmdir();
    volumes_lock.lock();
    if (VolumeLinks.erase(link->HostTarget))
        Statistics->VolumeLinksMounted--;
    link->HostTarget = "";
    volumes_lock.unlock();
    (void)Save();

    return error;
}

TError TVolume::UmountLink(std::shared_ptr<TVolumeLink> link,
                           std::list<std::shared_ptr<TVolume>> &unlinked,
                           bool strict) {
    TError error;

    auto volumes_lock = LockVolumes();
    if (link->Volume.get() != this)
        return TError(EError::InvalidValue, "Wrong volume link");
    if (!link->HostTarget)
        return OK;
    if (VolumeLinks.erase(link->HostTarget))
        Statistics->VolumeLinksMounted--;
    TPath host_target = link->HostTarget;
    link->HostTarget = "";

    volumes_lock.unlock();

    L_ACT("Umount volume {} link {} for CT{}:{}", Path, host_target, link->Container->Id, link->Container->Name);

    error = host_target.Umount(UMOUNT_NOFOLLOW | (strict ? 0 : MNT_DETACH));
    if (error) {
        error = TError(error, "Cannot umount volume {} link {} for CT{}:{}", Path, host_target, link->Container->Id, link->Container->Name);
        if (strict)
            goto undo;
    }

    /* Save changes only after umounting */
    (void)Save();

    /* Umount nested links */
    volumes_lock.lock();
    for (auto it = VolumeLinks.lower_bound(host_target);
            it != VolumeLinks.end() && it->first.IsInside(host_target);) {
        auto &link = it->second;

        L_ACT("Umount nested volume {} link {} for CT{}:{}", link->Volume->Path, link->HostTarget, link->Container->Id, link->Container->Name);

        /* common link */
        if (it->first == link->Volume->Path) {
            link->Volume->SetState(EVolumeState::Unlinked);
            unlinked.emplace_back(link->Volume);
        }

        link->HostTarget = "";
        it = VolumeLinks.erase(it);
        Statistics->VolumeLinksMounted--;
    }
    volumes_lock.unlock();

    return error;

undo:
    volumes_lock.lock();
    VolumeLinks[host_target] = link;
    Statistics->VolumeLinksMounted++;
    link->HostTarget = host_target;
    volumes_lock.unlock();
    return error;
}

void TVolume::DestroyAll() {
    std::list<std::shared_ptr<TVolume>> plan;
    for (auto &it : Volumes)
        plan.push_front(it.second);
    for (auto &vol: plan) {
        TError error = vol->Destroy();
        if (error)
            L_WRN("Cannot destroy volume {} : {}", vol->Path , error);
    }
}

TError TVolume::Destroy() {
    TError error, ret;

    std::list<std::shared_ptr<TVolume>> plan = {shared_from_this()};
    std::list<std::shared_ptr<TVolume>> work = {shared_from_this()};

    auto volumes_lock = LockVolumes();

    auto cycle = shared_from_this();

    bool stop_containers = HasDependentContainer;

    for (auto it = work.begin(); it != work.end(); ++it) {
        auto &volume = (*it);

        if (Path != volume->Path) {
            while (volume->State == EVolumeState::Building)
                VolumesCv.wait(volumes_lock);
        }

        while (volume->State == EVolumeState::Destroying)
            VolumesCv.wait(volumes_lock);

        if (volume->State != EVolumeState::Destroyed)
            volume->SetState(EVolumeState::ToDestroy);

        for (auto &nested : volume->Nested) {
            if (nested == cycle) {
                L_WRN("Cyclic dependencies for {} detected", cycle->Path);
            } else {
                auto plan_idx = std::find(plan.begin(), plan.end(), nested);

                if (plan_idx == plan.end())
                    cycle = nested;
                else
                    plan_idx = plan.erase(plan_idx);

                stop_containers |= nested->HasDependentContainer;

                plan.push_front(nested);

                work.push_back(nested);
            }
        }
    }

    if (stop_containers) {
        volumes_lock.unlock();

        for (auto &ct: RootContainer->Subtree()) {
            if (ct->RequiredVolumes.empty() || !ct->HasResources())
                continue;
            error = TVolume::CheckRequired(*ct);
            if (!error)
                continue;

            L_ACT("Stop CT{}:{} because {}", ct->Id, ct->Name, error);
            error = CL->LockContainer(ct);
            if (!error)
                error = ct->Stop(0);
            if (error)
                L_WRN("Cannot stop: {}", error);
            CL->ReleaseContainer();
        }

        volumes_lock.lock();
    }

    for (auto &volume : plan) {

        while (volume->State == EVolumeState::Destroying)
            VolumesCv.wait(volumes_lock);

        if (volume->State == EVolumeState::Destroyed)
            continue;

        volume->SetState(EVolumeState::Destroying);

        /* unlink containers and umount targets */
        while (!volume->Links.empty()) {
            auto link = volume->Links.back();
            volumes_lock.unlock();

            error = CL->LockContainer(link->Container);
            if (!error) {
                error = volume->UnlinkVolume(link->Container, link->Target, plan);
                CL->ReleaseContainer();
            }

            volumes_lock.lock();
            if (error && link == volume->Links.back()) {
                L_WRN("Cannot unlink volume {}: {}", volume->Path, error);
                volume->Links.remove(link);
            }
        }

        volumes_lock.unlock();

        /* wait in-progress operations */
        auto state_lock = volume->LockState();
        state_lock.unlock();

        error = volume->DestroyOne();
        if (error && !ret)
            ret = error;

        error = volume->ClaimPlace(0);
        if (error)
            L_WRN("Cannot free claimed space: {}", error);

        volumes_lock.lock();

        for (auto &it : Volumes)
            it.second->Nested.erase(volume);

        Volumes.erase(volume->Path);

        /* Remove common link */
        if (VolumeLinks.erase(volume->Path))
            Statistics->VolumeLinksMounted--;

        if (volume->VolumeOwnerContainer) {
            volume->VolumeOwnerContainer->OwnedVolumes.remove(volume);
            volume->VolumeOwnerContainer = nullptr;
        }

        volume->SetState(EVolumeState::Destroyed);

        volumes_lock.unlock();

        for (auto &layer: volume->Layers) {
            TStorage storage(volume->Place, PORTO_LAYERS, layer);
            if (StringStartsWith(layer, PORTO_WEAK_PREFIX)) {
                error = storage.Remove();
                if (error && error != EError::Busy)
                    L_ERR("Cannot remove weak layer {} : {}", layer, error);
            } else if (layer[0] != '/')
                (void)storage.Touch();
        }

        volumes_lock.lock();
    }

    return ret;
}

TError TVolume::DestroyOne() {
    L_ACT("Destroy volume: {} backend: {}", Path, BackendType);

    TPath internal = GetInternal("");
    TError ret, error;

    if (Path != InternalPath) {
        error = Path.UmountAll();
        if (error) {
            L_ERR("Cannout umount volume: {}", error);
            if (!ret)
                ret = error;
        }
    }

    if (Backend) {
        error = Backend->Destroy();
        if (error) {
            L_ERR("Can't destroy volume backend: {}", error);
            if (!ret)
                ret = error;
        }
    }

    error = internal.UmountAll();
    if (error) {
        L_ERR("Cannot umount internal: {}", error);
        if (!ret)
            ret = error;
    }

    StorageFd.Close();

    if (KeepStorage && !UserStorage() && !RemoteStorage()) {
        error = TStorage(Place, PORTO_STORAGE, Storage).Touch();
        if (error)
            L_WRN("Cannot touch storage: {}", error);
    }

    if (!KeepStorage && !RemoteStorage() && StoragePath.Exists()) {
        if (!UserStorage()) {
            error = RemoveRecursive(StoragePath);
            if (error) {
                L_VERBOSE("Cannot remove storage {}: {}", StoragePath, error);
                error = StoragePath.RemoveAll();
                if (error) {
                    L_WRN("Cannot remove storage {}: {}", StoragePath, error);
                    if (!ret)
                        ret = error;
                }
            }
        } else {
            /* File image storage for backend=loop always persistent. */
            error = ClearRecursive(StoragePath);
            if (error)
                L_VERBOSE("Cannot clear storage {}: {}", StoragePath, error);
            error = StoragePath.ClearDirectory();
            if (error) {
                L_WRN("Cannot clear storage {}: {}", StoragePath, error);
                if (!ret)
                    ret = error;
            }
        }
    }

    if (IsAutoPath && Path.Exists()) {
        error = Path.RemoveAll();
        if (error) {
            L_ERR("Cannot remove volume path: {}", error);
            if (!ret)
                ret = error;
        }
    }

    if (internal.Exists()) {
        error = internal.RemoveAll();
        if (error) {
            L_ERR("Cannot remove internal: {}", error);
            if (!ret)
                ret = error;
        }
    }

    TPath node(VolumesKV / Id);
    auto volumes_lock = LockVolumes();
    error = node.Unlink();
    volumes_lock.unlock();
    if (!ret && error)
        ret = error;

    return ret;
}

TError TVolume::StatFS(TStatFS &result) {
    auto lock = LockState();
    if (State != EVolumeState::Ready && State != EVolumeState::Unlinked) {
        result.Reset();
        return TError(EError::VolumeNotReady, "Volume not ready: " + Path.ToString());
    }
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

    auto lock = LockState();
    if (State != EVolumeState::Ready)
        return TError(EError::VolumeNotReady, "Volume not ready: " + Path.ToString());

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

        if (!spaceLimit || spaceLimit > SpaceLimit) {
            error = ClaimPlace(spaceLimit);
            if (error)
                return error;
        }

        L_ACT("Resize volume: {} to bytes: {} inodes: {}",
                Path, spaceLimit, inodeLimit);

        error = Backend->Resize(spaceLimit, inodeLimit);
        if (error) {
            if (!spaceLimit || spaceLimit > SpaceLimit)
                ClaimPlace(SpaceLimit);
            return error;
        }

        if (spaceLimit && spaceLimit < SpaceLimit)
            ClaimPlace(spaceLimit);

        auto volumes_lock = LockVolumes();
        SpaceLimit = spaceLimit;
        InodeLimit = inodeLimit;
        volumes_lock.unlock();
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

        auto volumes_lock = LockVolumes();
        SpaceGuarantee = space_guarantee;
        InodeGuarantee = inode_guarantee;
        volumes_lock.unlock();
    }

    return Save();
}

TError TVolume::GetUpperLayer(TPath &upper) {
    if (BackendType == "overlay")
        upper = StoragePath / "upper";
    else
        upper = Path;
    return OK;
}

TError TVolume::LinkVolume(std::shared_ptr<TContainer> container,
                           const TPath &target, bool read_only, bool required) {

    PORTO_ASSERT(container->IsLocked());

    if (target && (!target.IsAbsolute() || !target.IsNormal()))
        return TError(EError::InvalidValue, "Non-normalized target path {}", target);

    auto volumes_lock = LockVolumes();
    TError error;
    TPath host_target;

    for (auto &link: Links) {
        if (link->Container == container && link->Target == target)
            return TError(EError::VolumeAlreadyLinked, "Volume already linked");
    }

    if (target && container->HasResources()) {
        host_target = container->RootPath / target;
        error = CheckConflicts(host_target);
        if (error)
            return error;
    }

    if (State != EVolumeState::Ready)
        return TError(EError::VolumeNotReady, "Volume not ready: {}", Path);

    L_ACT("Add volume {} link {} for CT{}:{}", Path, target, container->Id, container->Name);

    auto link = std::make_shared<TVolumeLink>(shared_from_this(), container);
    link->Target = target;
    link->ReadOnly = read_only;
    link->Required = required;
    link->HostTarget = host_target;   /* protect path from conflicts */

    Links.emplace_back(link);
    container->VolumeLinks.emplace_back(link);

    if (required) {
        container->RequiredVolumes.emplace_back(Path.ToString());
        HasDependentContainer = true;
    }
    volumes_lock.unlock();

    error = Save();
    if (error)
        goto undo;

    if (host_target) {
        error = MountLink(link);
        if (error)
            goto undo;
    }

    if (required) {
        container->SetProp(EProperty::REQUIRED_VOLUMES);
        (void)container->Save();
    }

    return OK;

undo:
    volumes_lock.lock();
    Links.remove(link);
    container->VolumeLinks.remove(link);
    if (required) {
        auto it = std::find(container->RequiredVolumes.begin(),
                            container->RequiredVolumes.end(), target);
        if (it != container->RequiredVolumes.end())
            container->RequiredVolumes.erase(it);
    }
    volumes_lock.unlock();
    return error;
}

TError TVolume::UnlinkVolume(std::shared_ptr<TContainer> container, const TPath &target,
                             std::list<std::shared_ptr<TVolume>> &unlinked, bool strict) {

    PORTO_ASSERT(container->IsLocked());

    TError error;
    auto volumes_lock = LockVolumes();
    std::shared_ptr<TVolumeLink> link;
    bool all = target.ToString() == "***";

next:
    for (auto &l: Links) {
        if (l->Container == container && (all || l->Target == target)) {
            link = l;
            break;
        }
    }
    if (!link) {
        if (all)
            return OK;
        return TError(EError::VolumeNotLinked, "Container {} is not linked with volume {}", container->Name, Path);
    }

    /* If strict then fail if volume is used */

    if (strict && Links.size() > 1)
        return TError(EError::Busy, "More than one container linked with volume {}", Path);

    if (strict && !Nested.empty())
        return TError(EError::Busy, "Volume {} has sub-volumes", Path);

    if (link->HostTarget) {
        volumes_lock.unlock();
        error = UmountLink(link, unlinked, strict);
        if (error) {
            if (strict)
                return error;
            L_WRN("Cannot umount volume link: {}", error);
        }
        volumes_lock.lock();
    }

    L_ACT("Del volume {} link {} for CT{}:{}", Path, link->Target, container->Id, container->Name);

    Links.remove(link);
    if (Links.empty() && State == EVolumeState::Ready) {
        SetState(EVolumeState::Unlinked);
        unlinked.emplace_back(shared_from_this());
    }
    container->VolumeLinks.remove(link);
    if (link->Required) {
        auto it = std::find(container->RequiredVolumes.begin(),
                            container->RequiredVolumes.end(), target);
        if (it != container->RequiredVolumes.end())
            container->RequiredVolumes.erase(it);
    }
    volumes_lock.unlock();
    link.reset();

    (void)Save();

    if (all) {
        volumes_lock.lock();
        goto next;
    }

    return OK;
}

void TVolume::UnlinkAllVolumes(std::shared_ptr<TContainer> container,
                               std::list<std::shared_ptr<TVolume>> &unlinked) {
    auto volumes_lock = LockVolumes();
    TError error;

    while (!container->VolumeLinks.empty()) {
        std::shared_ptr<TVolumeLink> link = container->VolumeLinks.back();
        volumes_lock.unlock();
        error = link->Volume->UnlinkVolume(container, link->Target, unlinked);
        volumes_lock.lock();
        if (error && link == container->VolumeLinks.back()) {
            L_WRN("Cannot unlink volume {}: {}", link->Volume->Path, error);
            container->VolumeLinks.remove(link);
        }
    }

    for (auto &volume: container->OwnedVolumes) {
        volume->VolumeOwnerContainer = container->Parent;
        if (volume->VolumeOwnerContainer)
            volume->VolumeOwnerContainer->OwnedVolumes.push_back(volume);
    }
    container->OwnedVolumes.clear();
}

void TVolume::DestroyUnlinked(std::list<std::shared_ptr<TVolume>> &unlinked) {
    TError error;

    for (auto &volume: unlinked) {
        error = volume->Destroy();
        if (error)
            L_WRN("Cannot destroy volume {}: {}", volume->Path, error);
    }
}

TError TVolume::CheckRequired(TContainer &ct) {
    auto volumes_lock = LockVolumes();
    for (auto &path: ct.RequiredVolumes) {
        auto link = ResolveLinkLocked(ct.RootPath / path);
        if (!link)
            return TError(EError::VolumeNotFound, "Required volume {} not found", path);
        if (link->Volume->State != EVolumeState::Ready)
            return TError(EError::VolumeNotReady, "Required volume {} not ready", path);
        link->Volume->HasDependentContainer = true;
    }
    return OK;
}

void TVolume::Dump(TVolumeLink *link, const TPath &path, rpc::TVolumeDescription *dump) {
    TStringMap ret;

    auto volumes_lock = LockVolumes();

    ret[V_ID] = Id;

    if (UserStorage() && !RemoteStorage())
        ret[V_STORAGE] = CL->ComposePath(StoragePath).ToString();
    else
        ret[V_STORAGE] = Storage;

    ret[V_BACKEND] = BackendType;

    if (VolumeOwnerContainer)
        ret[V_OWNER_CONTAINER] = CL->RelativeName(VolumeOwnerContainer->Name);

    ret[V_OWNER_USER] = VolumeOwner.User();
    ret[V_OWNER_GROUP] = VolumeOwner.Group();
    if (VolumeCred.Uid != NoUser)
        ret[V_USER] = VolumeCred.User();
    if (VolumeCred.Gid != NoGroup)
        ret[V_GROUP] = VolumeCred.Group();
    if (VolumePerms)
        ret[V_PERMISSIONS] = StringFormat("%#o", VolumePerms);
    ret[V_CREATOR] = Creator;
    ret[V_READY] = BoolToString(State == EVolumeState::Ready);
    if (BuildTime.size())
        ret[V_BUILD_TIME] = BuildTime;
    ret[V_STATE] = StateName(State);
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
                l = CL->ComposePath(path).ToString();
        }
        ret[V_LAYERS] = MergeEscapeStrings(layers, ';');
    }

    if (CustomPlace)
        ret[V_PLACE] = Place.ToString();

    if (Backend)
        ret[V_PLACE_KEY] = Backend->ClaimPlace();

    /* common link is pinned by all links */
    if (!link || link->HostTarget == Path) {
        for (auto &link: Links) {
            auto name = CL->RelativeName(link->Container->Name);
            dump->add_containers(name);
            auto l = dump->add_links();
            l->set_container(name);
            if (link->Target)
                l->set_target(link->Target.ToString());
            if (link->ReadOnly)
                l->set_read_only(true);
            if (link->Required)
                l->set_required(true);
        }
    } else {
        auto name = CL->RelativeName(link->Container->Name);
        dump->add_containers(name);
        auto l = dump->add_links();
        l->set_container(name);
        if (link->Target)
            l->set_target(link->Target.ToString());
        if (link->ReadOnly)
            l->set_read_only(true);
        if (link->Required)
            l->set_required(true);
    }

    volumes_lock.unlock();

    TStatFS stat;
    if (!StatFS(stat)) {
        ret[V_SPACE_USED] = std::to_string(stat.SpaceUsage);
        ret[V_INODE_USED] = std::to_string(stat.InodeUsage);
        ret[V_SPACE_AVAILABLE] = std::to_string(stat.SpaceAvail);
        ret[V_INODE_AVAILABLE] = std::to_string(stat.InodeAvail);
    }

    dump->set_path(path.ToString());

    for (auto &prop: ret) {
        auto p = dump->add_properties();
        p->set_name(prop.first);
        p->set_value(prop.second);
    }
}

TError TVolume::Save() {
    TKeyValue node(VolumesKV / Id);
    TError error;
    std::string tmp;

    auto volumes_lock = LockVolumes();

    if (State == EVolumeState::ToDestroy ||
            State == EVolumeState::Destroying ||
            State == EVolumeState::Destroyed)
        return OK;

    /*
     * Storing all state values on save,
     * the previous scheme stored knobs selectively.
     */

    node.Set(V_RAW_ID, Id);
    node.Set(V_PATH, Path.ToString());
    node.Set(V_AUTO_PATH, BoolToString(IsAutoPath));
    node.Set(V_STORAGE, Storage);
    node.Set(V_BACKEND, BackendType);

    if (VolumeOwnerContainer) {
        /*
         * Older porto versions afraid volumes with unknown properties.
         * Save owner container into first word in creator.
         */
        if (config().volumes().owner_container_migration_hack()) {
            auto creator = SplitEscapedString(Creator, ' ');
            creator[0] = VolumeOwnerContainer->Name;
            node.Set(V_CREATOR, MergeEscapeStrings(creator, ' '));
        } else {
            node.Set(V_CREATOR, Creator);
            node.Set(V_OWNER_CONTAINER, VolumeOwnerContainer->Name);
        }
    }

    node.Set(V_OWNER_USER, VolumeOwner.User());
    node.Set(V_OWNER_GROUP, VolumeOwner.Group());
    if (VolumeCred.Uid != NoUser)
        node.Set(V_USER, VolumeCred.User());
    if (VolumeCred.Gid != NoGroup)
        node.Set(V_GROUP, VolumeCred.Group());
    if (VolumePerms)
        node.Set(V_PERMISSIONS, StringFormat("%#o", VolumePerms));
    node.Set(V_READY, BoolToString(State == EVolumeState::Ready));
    node.Set(V_PRIVATE, Private);
    node.Set(V_LOOP_DEV, std::to_string(Device));
    node.Set(V_READ_ONLY, BoolToString(IsReadOnly));
    node.Set(V_LAYERS, MergeEscapeStrings(Layers, ';'));
    node.Set(V_SPACE_LIMIT, std::to_string(SpaceLimit));
    node.Set(V_SPACE_GUARANTEE, std::to_string(SpaceGuarantee));
    node.Set(V_INODE_LIMIT, std::to_string(InodeLimit));
    node.Set(V_INODE_GUARANTEE, std::to_string(InodeGuarantee));

    TMultiTuple links;
    for (auto &link: Links) {
        if (link->Target)
            links.push_back({link->Container->Name,
                             link->Target.ToString(),
                             link->ReadOnly ? "ro" : "rw",
                             link->HostTarget.ToString()});
        else
            links.push_back({link->Container->Name});
    }
    node.Set(V_RAW_CONTAINERS, MergeEscapeStrings(links, ' ', ';'));

    if (CustomPlace)
        node.Set(V_PLACE, Place.ToString());

    error = node.Save();
    if (error)
        L_WRN("Cannot save volume {} {}", Path, error);

    return error;
}

TError TVolume::Restore(const TKeyValue &node) {
    if (!node.Has(V_RAW_ID))
        return TError(EError::InvalidValue, "No volume id stored");

    TError error = ApplyConfig(node.Data);
    if (error)
        return error;

    InternalPath = Place / PORTO_VOLUMES / Id / "volume";

    auto creator = SplitEscapedString(Creator, ' ');

    if (!node.Has(V_OWNER_USER)) {
        if (creator.size() != 3 ||
                UserId(creator[1], VolumeOwner.Uid) ||
                GroupId(creator[2], VolumeOwner.Gid))
            VolumeOwner = VolumeCred;
    }

    error = CL->WriteContainer(node.Has(V_OWNER_CONTAINER) ?
                               node.Get(V_OWNER_CONTAINER) : creator[0],
                               VolumeOwnerContainer, true);
    if (error) {
        L_WRN("Cannot find volume owner: {}", error);
        VolumeOwnerContainer = RootContainer;
    }
    VolumeOwnerContainer->OwnedVolumes.push_back(shared_from_this());

    if (!HaveStorage())
        StoragePath = GetInternal(BackendType);
    else if (!UserStorage() && !RemoteStorage())
        StoragePath = TStorage(Place, PORTO_STORAGE, Storage).Path;

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Restore();
    if (error)
        return error;

    error = ClaimPlace(SpaceLimit);
    if (error)
        return error;

    return OK;
}

std::vector<TVolumeProperty> VolumeProperties = {
    { V_BACKEND,     "dir|plain|bind|rbind|tmpfs|quota|native|overlay|squash|lvm|loop|rbd (default - autodetect)", false },
    { V_STORAGE,     "path to data storage (default - internal)", false },
    { V_READY,       "true|false - contruction complete (ro)", true },
    { V_STATE,       "volume state (ro)", true },
    { V_PRIVATE,     "user-defined property", false },
    { V_TARGET_CONTAINER, "target container (default - self)", false },
    { V_OWNER_CONTAINER, "owner container (default - self)", false },
    { V_OWNER_USER,  "owner user (default - creator)", false },
    { V_OWNER_GROUP, "owner group (default - creator)", false },
    { V_USER,        "directory user (default - creator)", false },
    { V_GROUP,       "directory group (default - creator)", false },
    { V_PERMISSIONS, "directory permissions (default - 0775)", false },
    { V_CREATOR,     "container user group (ro)", true },
    { V_READ_ONLY,   "true|false (default - false)", false },
    { V_CONTAINERS,  "container[=target];... - initial links (default - self)", false },
    { V_LAYERS,      "top-layer;...;bottom-layer - overlayfs layers", false },
    { V_PLACE,       "place for layers and default storage (optional)", false },
    { V_PLACE_KEY,   "key for charging place_limit for owner_container (ro)", true },
    { V_SPACE_LIMIT, "disk space limit (dynamic, default zero - unlimited)", false },
    { V_INODE_LIMIT, "disk inode limit (dynamic, default zero - unlimited)", false },
    { V_SPACE_GUARANTEE,    "disk space guarantee (dynamic, default - zero)", false },
    { V_INODE_GUARANTEE,    "disk inode guarantee (dynamic, default - zero)", false },
    { V_SPACE_USED,  "current disk space usage (ro)", true },
    { V_INODE_USED,  "current disk inode used (ro)", true },
    { V_SPACE_AVAILABLE,    "available disk space (ro)", true },
    { V_INODE_AVAILABLE,    "available disk inodes (ro)", true },
};

TError TVolume::Create(const TStringMap &cfg, std::shared_ptr<TVolume> &volume) {
    TError error;

    if (!CL)
        return TError("no client");

    if (cfg.count(V_PLACE)) {
        error = CL->CanControlPlace(cfg.at(V_PLACE));
        if (error)
            return error;
        error = TStorage::CheckPlace(cfg.at(V_PLACE));
        if (error)
            return error;
    } else {
        error = CL->CanControlPlace(PORTO_PLACE);
        if (error)
            return error;
    }

    std::shared_ptr<TContainer> owner;
    TPath target_root;

    if (cfg.count(V_TARGET_CONTAINER)) {
        std::shared_ptr<TContainer> target;
        error = CL->WriteContainer(cfg.at(V_TARGET_CONTAINER), target, true);
        if (error)
            return error;
        if (target->State == EContainerState::Stopped)
            return TError(EError::InvalidState, "Target container is stopped");
        target_root = target->RootPath;
    } else
        target_root = CL->ClientContainer->RootPath;

    if (cfg.count(V_OWNER_CONTAINER)) {
        error = CL->WriteContainer(cfg.at(V_OWNER_CONTAINER), owner, true);
    } else {
        owner = CL->ClientContainer;
        error = CL->LockContainer(owner);
    }
    if (error)
        return error;

    auto volumes_lock = LockVolumes();

    auto max_vol = config().volumes().max_total();
    if (CL->IsSuperUser())
        max_vol += NR_SUPERUSER_VOLUMES;

    if (Volumes.size() >= max_vol)
        return TError(EError::ResourceNotAvailable, "number of volumes reached limit: " + std::to_string(max_vol));

    if (cfg.count(V_PATH)) {
        TPath path = cfg.at(V_PATH);

        if (!path.IsAbsolute())
            return TError(EError::InvalidValue, "Volume path must be absolute");

        if (!path.IsNormal())
            return TError(EError::InvalidValue, "Volume path must be normalized");

        path = target_root / path;

        if (Volumes.count(path))
            return TError(EError::VolumeAlreadyExists, "Volume already exists");

        if (!path.Exists())
            return TError(EError::InvalidValue, "Volume path does not exist");

        if (!path.IsDirectoryStrict())
            return TError(EError::InvalidValue, "Volume path must be a directory");

        error = CheckConflicts(path);
        if (error)
            return error;
    }

    volume = std::make_shared<TVolume>();
    volume->Id = std::to_string(NextId++);

    error = volume->Configure(target_root, cfg);
    if (error)
        return error;

    /* Add common link */
    auto common_link = std::make_shared<TVolumeLink>(volume, RootContainer);
    common_link->Target = volume->Path;
    common_link->HostTarget = volume->Path;
    common_link->ReadOnly = volume->IsReadOnly;
    VolumeLinks[volume->Path] = common_link;

    /* also check if volume depends on itself */
    error = volume->CheckDependencies();
    if (error) {
        VolumeLinks.erase(volume->Path);
        return error;
    }

    Volumes[volume->Path] = volume;
    Statistics->VolumeLinksMounted++;

    volume->VolumeOwnerContainer = owner;
    owner->OwnedVolumes.push_back(volume);

    volume->SetState(EVolumeState::Building);

    volumes_lock.unlock();

    error = volume->ClaimPlace(volume->SpaceLimit);
    if (error) {
        (void)volume->Destroy();
        return error;
    }

    error = volume->Build();
    if (error) {
        (void)volume->Destroy();
        return error;
    }

    volumes_lock.lock();
    volume->SetState(EVolumeState::Ready);
    volumes_lock.unlock();

    /* And finally, mount common link in requested path */
    if (volume->Path != volume->InternalPath) {
        error = volume->MountLink(common_link);
        if (error) {
            (void)volume->Destroy();
            return error;
        }
    }

    if (cfg.count(V_CONTAINERS)) {
        for (auto &link: SplitEscapedString(cfg.at(V_CONTAINERS), ' ', ';')) {
            std::shared_ptr<TContainer> ct;
            error = CL->WriteContainer(link[0], ct, true);
            if (error)
                break;
            auto target = link.size() > 1 ? link[1] : "";
            bool ro = link.size() > 2 && link[2] == "ro";
            error = volume->LinkVolume(ct, target, ro);
            if (error)
                break;
        }
    } else {
        error = volume->LinkVolume(CL->ClientContainer);
    }
    if (error) {
        (void)volume->Destroy();
        return error;
    }

    error = volume->Save();
    if (error) {
        (void)volume->Destroy();
        return error;
    }

    return OK;
}

void TVolume::RestoreAll(void) {
    std::list<TKeyValue> nodes;
    TError error;

    TPath place(PORTO_PLACE);
    error = TStorage::CheckPlace(place);
    if (error)
        L_ERR("Cannot prepare place: {}", error);

    error = TKeyValue::ListAll(VolumesKV, nodes);
    if (error)
        L_ERR("Cannot list nodes: {}", error);

    for (auto &node : nodes) {
        error = node.Load();
        if (error) {
            L_WRN("Cannot load {} removed: {}", node.Path, error);
            node.Path.Unlink();
            continue;
        }

        /* key for sorting */
        node.Name = node.Get(V_RAW_ID);
        node.Name.insert(0, 20 - node.Name.size(), '0');
    }

    nodes.sort();

    std::list<std::shared_ptr<TVolume>> broken_volumes;

    for (auto &node : nodes) {
        if (!node.Name.size())
            continue;

        auto volume = std::make_shared<TVolume>();

        L_ACT("Restore volume: {}", node.Path);
        error = volume->Restore(node);
        if (error) {
            /* Apparently, we cannot trust node contents, remove right away */
            L_WRN("Corrupted volume {} removed: {}", node.Path, error);
            (void)volume->Destroy();
            continue;
        }

        uint64_t id;
        if (!StringToUint64(volume->Id, id)) {
            if (id >= NextId)
                NextId = id + 1;
        }

        Volumes[volume->Path] = volume;

        /* Restore common link */
        auto common_link = std::make_shared<TVolumeLink>(volume, RootContainer);
        common_link->Target = volume->Path;
        common_link->HostTarget = volume->Path;
        common_link->ReadOnly = volume->IsReadOnly;
        VolumeLinks[volume->Path] = common_link;
        Statistics->VolumeLinksMounted++;

        error = volume->Save();
        if (error) {
            broken_volumes.push_back(volume);
            continue;
        }

        error = volume->CheckDependencies();
        if (error) {
            L_WRN("Volume {} has broken dependcies: {}", volume->Path, error);
            broken_volumes.push_back(volume);
            continue;
        }

        if (volume->State != EVolumeState::Ready) {
            L("Volume {} is not ready ({}) and will be removed", volume->Path, StateName(volume->State));
            broken_volumes.push_back(volume);
            continue;
        }

        if (!volume->Links.size()) {
            L_WRN("Volume {} has no linked containers", volume->Path);
            broken_volumes.push_back(volume);
            continue;
        }

        L("Volume {} restored", volume->Path);
    }

    L_SYS("Remove broken volumes...");

    for (auto &volume : broken_volumes) {
        if (volume->State != EVolumeState::Ready)
            (void)volume->Destroy();
    }

    TPath volumes = place / PORTO_VOLUMES;

    L_SYS("Remove stale volumes...");

    std::vector<std::string> subdirs;
    error = volumes.ReadDirectory(subdirs);
    if (error)
        L_ERR("Cannot list {}", volumes);

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

        error = dir.UmountNested();
        if (error)
            L_ERR("Cannot umount nested : {}", error);

        error = RemoveRecursive(dir);
        if (error) {
            L_VERBOSE("Cannot remove {}: {}", dir, error);
            error = dir.RemoveAll();
            if (error)
                L_WRN("Cannot remove {}: {}", dir, error);
        }
    }

    L_SYS("Remove stale weak layers...");

    std::list<TStorage> layers;

    error = TStorage::List(place, PORTO_LAYERS, layers);
    if (!error) {
        for (auto &layer : layers) {
            if (StringStartsWith(layer.Name, PORTO_WEAK_PREFIX)) {

                error = layer.Remove();
                if (error && error != EError::Busy)
                    L_ERR("Cannot remove weak layer {} : {}", layer.Name, error);
            }
        }
    } else {
        L_WRN("Layers listing failed : {}", error);
    }
}

TError TVolume::ApplyConfig(const TStringMap &cfg) {
    TError error;

    for (auto &prop : cfg) {
        if (prop.first == V_PATH) {
            Path = prop.second;

        } else if (prop.first == V_AUTO_PATH) {
            error = StringToBool(prop.second, IsAutoPath);
            if (error)
                return error;

        } else if (prop.first == V_STORAGE) {
            Storage = prop.second;
            StoragePath = Storage;
            KeepStorage = HaveStorage();

        } else if (prop.first == V_BACKEND) {
            BackendType = prop.second;

        } else if (prop.first == V_TARGET_CONTAINER) {

        } else if (prop.first == V_OWNER_CONTAINER) {

        } else if (prop.first == V_OWNER_USER) {
            error = UserId(prop.second, VolumeOwner.Uid);
            if (error)
                return error;

        } else if (prop.first == V_OWNER_GROUP) {
            error = GroupId(prop.second, VolumeOwner.Gid);
            if (error)
                return error;

        } else if (prop.first == V_USER) {
            error = UserId(prop.second, VolumeCred.Uid);
            if (error)
                return error;

        } else if (prop.first == V_GROUP) {
            error = GroupId(prop.second, VolumeCred.Gid);
            if (error)
                return error;

        } else if (prop.first == V_PERMISSIONS) {
            error = StringToOct(prop.second, VolumePerms);
            if (error)
                return error;

        } else if (prop.first == V_CREATOR) {
            Creator = prop.second;

        } else if (prop.first == V_RAW_ID) {
            Id = prop.second;

        } else if (prop.first == V_READY) {
            bool ready;
            error = StringToBool(prop.second, ready);
            if (error)
                return error;

            SetState(ready ? EVolumeState::Ready : EVolumeState::ToDestroy);

        } else if (prop.first == V_BUILD_TIME) {
            BuildTime = prop.second;

        } else if (prop.first == V_PRIVATE) {
            Private = prop.second;

        } else if (prop.first == V_RAW_CONTAINERS) {
            auto containers_lock = LockContainers();
            TError ret;

            /* container target ro|rw host-target */
            for (auto &l: SplitEscapedString(prop.second, ' ', ';')) {
                std::shared_ptr<TContainer> ct;
                error = TContainer::Find(l[0], ct);
                if (error) {
                    if (!ret)
                        ret = error;
                    ct = RootContainer; /* placeholder */
                }
                auto link = std::make_shared<TVolumeLink>(shared_from_this(), ct);
                if (l.size() > 1)
                    link->Target = l[1];
                if (l.size() > 2)
                    link->ReadOnly = l[2] == "ro";
                if (l.size() > 3) {
                    link->HostTarget = l[3];
                    Statistics->VolumeLinksMounted++;
                }
                Links.emplace_back(link);
                ct->VolumeLinks.emplace_back(link);
            }

            if (ret)
                return ret;

        } else if (prop.first == V_CONTAINERS) {

        } else if (prop.first == V_LOOP_DEV) {
            error = StringToInt(prop.second, Device);
            if (error)
                return error;

        } else if (prop.first == V_READ_ONLY) {
            error = StringToBool(prop.second, IsReadOnly);
            if (error)
                return error;

        } else if (prop.first == V_LAYERS) {
            Layers = SplitEscapedString(prop.second, ';');

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

        } else
            L_WRN("Skip unknown volume property {} = {}", prop.first, prop.second);
    }

    return OK;
}
