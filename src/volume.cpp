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
static uint64_t NextId = 1;

static std::condition_variable VolumesCv;

/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure() {
    return TError::Success();
}

TError TVolumeBackend::Save() {
    return TError::Success();
}

TError TVolumeBackend::Restore() {
    return TError::Success();
}

TError TVolumeBackend::Resize(uint64_t, uint64_t) {
    return TError(EError::NotSupported, "not implemented");
}

/* TVolumePlainBackend - bindmount */

class TVolumePlainBackend : public TVolumeBackend {
public:

    TError Configure() override {

        if (Volume->HaveQuota())
            return TError(EError::InvalidProperty, "Plain backend have no support of quota");

        return TError::Success();
    }

    TError Build() override {
        return Volume->InternalPath.BindRemount(Volume->StoragePath,
                                        Volume->GetMountFlags());
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

        return TError::Success();
    }

    TError Build() override {
        return Volume->InternalPath.BindRemount(Volume->StoragePath,
                                                Volume->GetMountFlags());
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

        return TError::Success();
    }

    TError Build() override {
        std::vector<std::string> opts;

        if (Volume->SpaceLimit)
            opts.emplace_back("size=" + std::to_string(Volume->SpaceLimit));

        if (Volume->InodeLimit)
            opts.emplace_back("nr_inodes=" + std::to_string(Volume->InodeLimit));

        return Volume->InternalPath.Mount("porto:" + Volume->Id, "tmpfs",
                                          Volume->GetMountFlags(), opts);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        std::vector<std::string> opts;

        if (space_limit)
            opts.emplace_back("size=" + std::to_string(space_limit));

        if (inode_limit)
            opts.emplace_back("nr_inodes=" + std::to_string(inode_limit));

        return Volume->InternalPath.Mount("porto:" + Volume->Id, "tmpfs",
                                          Volume->GetMountFlags() | MS_REMOUNT,
                                          opts);
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

    static bool Supported() {
        static bool supported = false, tested = false;

        if (!config().volumes().enable_quota())
            return false;

        if (!tested) {
            TProjectQuota quota = TPath(PORTO_PLACE) / PORTO_VOLUMES;
            supported = quota.Supported();
            if (supported)
                L_SYS("Project quota is supported: {}", quota.Path.c_str());
            else
                L_SYS("Project quota not supported: {}", quota.Path.c_str());
            tested = true;
        }

        return supported;
    }

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

        return TError::Success();
    }

    TError Restore() {

        /* Restore configuration */
        Volume->InternalPath = Volume->Path;
        Volume->StoragePath = Volume->Path;
        Volume->KeepStorage = true;

        return TError::Success();
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

    static bool Supported() {
        return TVolumeQuotaBackend::Supported();
    }

    TError Configure() override {

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::NotSupported, "project quota is disabled");

        return TError::Success();
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
                                                Volume->GetMountFlags());
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
        L_WRN("Cannot enable O_DIRECT for loop {}", TError(EError::Unknown, errno, "fcntl"));

    auto lock = std::unique_lock<std::mutex>(BigLoopLock);

again:
    nr = ioctl(ctl.Fd, LOOP_CTL_GET_FREE);
    if (nr < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_CTL_GET_FREE)");

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
        return TError(EError::Unknown, errno, "ioctl(LOOP_SET_FD)");
    }

    memset(&info, 0, sizeof(info));
    strncpy((char *)info.lo_file_name, path.c_str(), LO_NAME_SIZE - 1);

    if (ioctl(dev.Fd, LOOP_SET_STATUS64, &info) < 0) {
        error = TError(EError::Unknown, errno, "ioctl(LOOP_SET_STATUS64)");
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
        return TError(EError::Unknown, errno, "ioctl(LOOP_CLR_FD)");
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

        return TError::Success();
    }

    TPath GetLoopDevice() {
        if (Volume->Device < 0)
            return TPath();
        return TPath("/dev/loop" + std::to_string(Volume->Device));
    }

    static TError MakeImage(TFile &file, TPath &path, off_t size, off_t guarantee) {
        TError error;

        L_ACT("Allocate loop image with size {} guarantee {}", size, guarantee);

        if (ftruncate(file.Fd, size))
            return TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");

        if (guarantee && fallocate(file.Fd, FALLOC_FL_KEEP_SIZE, 0, guarantee))
            return TError(EError::ResourceNotAvailable, errno,
                           "cannot fallocate guarantee " + std::to_string(guarantee));

        return RunCommand({ "mkfs.ext4", "-q", "-F", "-m", "0", "-E", "nodiscard",
                            "-O", "^has_journal", path.ToString()}, path.DirName());
    }

    static TError ResizeImage(const TFile &file, const TPath &path,
                              off_t current, off_t target) {
        std::string size = std::to_string(target >> 10) + "K";
        TError error;

        if (current < target && ftruncate(file.Fd, target))
            return TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");

        error = RunCommand({"resize2fs", "-f", path.ToString(), size},
                           path.DirName().ToString());

        if (!error && current > target && ftruncate(file.Fd, target))
            error = TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");

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
            return TError(EError::Unknown, errno, "ioctl(LOOP_SET_CAPACITY)");

        return RunCommand({"resize2fs", path, size}, "/");
    }

    TError Build() override {
        TPath path = ImagePath(Volume->StoragePath);
        struct stat st;
        TError error;
        TFile file;

        if (!Volume->StorageFd.Stat(st) && S_ISREG(st.st_mode)) {
            error = file.Dup(Volume->StorageFd);
        } else if (!Volume->StorageFd.StatAt(AutoImage, true, st) && S_ISREG(st.st_mode)) {
            error = file.OpenAt(Volume->StorageFd, AutoImage,
                                (Volume->IsReadOnly ? O_RDONLY : O_RDWR) |
                                O_CLOEXEC | O_NOCTTY, 0);
        } else {
            error = file.OpenAt(Volume->StorageFd, AutoImage,
                                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (!error) {
                Volume->KeepStorage = false; /* New storage */
                error = MakeImage(file, path, Volume->SpaceLimit, Volume->SpaceGuarantee);
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
            error = ResizeImage(file, path, st.st_size, Volume->SpaceLimit);
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
            return TError::Success();

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

        return TError::Success();
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

        if (unshare(CLONE_FS))
            return TError(EError::Unknown, errno, "unshare(CLONE_FS)");

        for (auto &name: Volume->Layers) {
            TPath path, temp;
            TFile pin;

            if (name[0] == '/') {
                error = pin.OpenDir(name);
                if (error)
                    goto err;
                error = CL->WriteAccess(pin);
                if (error) {
                    error = TError(error, "Layer " + name);
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
                error = temp.BindRemount(path, MS_RDONLY | MS_NODEV);
            if (!error)
                error = temp.Remount(MS_PRIVATE);
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

        if (error && error.GetErrno() == EINVAL && Volume->Layers.size() >= 500)
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

        if (Volume->Layers.size() != 1)
            return TError(EError::InvalidProperty, "Backend squash requires one image in layers");

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::InvalidProperty, "project quota is disabled");

        return TError::Success();
    }

    TError Build() override {
        TProjectQuota quota(Volume->StoragePath);
        TFile lowerFd, upperFd, workFd;
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

        if (Volume->HaveQuota()) {
            quota.SpaceLimit = Volume->SpaceLimit;
            quota.InodeLimit = Volume->InodeLimit;
            L_ACT("Creating project quota: {} bytes: {} inodes: {}",
                  quota.Path, quota.SpaceLimit, quota.InodeLimit);
            error = quota.Create();
            if (error)
                  goto err;
        }

        error = Volume->InternalPath.Mount("overlay", "overlay",
                                   Volume->GetMountFlags(),
                                   { "lowerdir=" + lower.ToString(),
                                     "upperdir=" + upperFd.ProcPath().ToString(),
                                     "workdir=" + workFd.ProcPath().ToString() });
        if (!error)
            return error;

err:
        if (Volume->HaveQuota())
            (void)quota.Destroy();
        if (Volume->Device >= 0) {
            lower.UmountAll();
            PutLoopDev(Volume->Device);
            Volume->Device = -1;
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
        return TError::Success();
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

        error = out.CreateTemp("/tmp");
        if (error)
            return error;
        error = RunCommand({"rbd", "--id=" + id, "--pool=" + pool, "map", image}, "/", TFile(), out);
        if (error)
            return error;
        error = out.ReadAll(device, 1024);
        if (error)
            return error;
        device = StringTrim(device);
        return TError::Success();
    }

    TError UnmapDevice(std::string device) {
        L_ACT("Unmap rbd device {}", device);
        return RunCommand({"rbd", "unmap", device}, "/");
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
            return TError::Success();

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

    TError StatFS(TStatFS &result) override {
        return Volume->InternalPath.StatFS(result);
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

/* FIXME could be implemented with single lookup */
std::shared_ptr<TVolume> TVolume::Locate(const TPath &path) {
    if (path.IsAbsolute()) {
        for (auto p = path; !p.IsRoot(); p = p.DirName()) {
            auto volume = Find(p);
            if (volume)
                return volume;
        }
    }
    return nullptr;
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
    else if (BackendType == "squash")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeSquashBackend());
    else if (BackendType == "rbd")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeRbdBackend());
    else
        return TError(EError::NotSupported, "Unknown volume backend: " + BackendType);

    Backend->Volume = this;

    return TError::Success();
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

TError TVolume::CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee) const {
    TStatFS current, total;
    TPath storage;

    if (RemoteStorage() || (!space_guarantee && !inode_guarantee))
        return TError::Success();

    if (UserStorage())
        storage = StoragePath;
    else if (HaveStorage())
        storage = Place / PORTO_STORAGE;
    else
        storage = Place / PORTO_VOLUMES;

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

    if (BackendType != "loop" &&
            total.InodeAvail + current.InodeUsage + inode_claimed <
            inode_guarantee + inode_guaranteed)
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used " +
                      std::to_string(inode_claimed) + " claimed " +
                      std::to_string(inode_guaranteed) + " guaranteed");

    return TError::Success();
}

TError TVolume::DependsOn(const TPath &path) {
    if (IsReady && !path.Exists())
        return TError(EError::Unknown, "dependecy path " + path.ToString() + " not found");

    for (auto it = Volumes.rbegin(); it != Volumes.rend(); ++it) {
        auto &vol = it->second;
        if (path.IsInside(vol->Path)) {
            if (!vol->IsReady || vol->IsDying)
                return TError(EError::Busy, "Volume not ready: " + vol->Path.ToString());
            vol->Nested.insert(shared_from_this());
            break;
        }
    }

    return TError::Success();
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

TError TVolume::Configure(const TStringMap &cfg) {
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
        return TError(error, "Volume " + Path.ToString());

    if (VolumeOwner.Gid != CL->Cred.Gid && !CL->IsSuperUser() &&
            !CL->Cred.IsMemberOf(VolumeOwner.Gid))
        return TError(EError::Permission, "Changing owner group is not permitted");

    /* Autodetect volume backend */
    if (BackendType == "") {
        if ((HaveQuota() && !TVolumeNativeBackend::Supported()) ||
                StoragePath.IsRegularFollow())
            BackendType = "loop";
        else if (HaveLayers() && TVolumeOverlayBackend::Supported())
            BackendType = "overlay";
        else if (TVolumeNativeBackend::Supported())
            BackendType = "native";
        else
            BackendType = "plain";
    }

    InternalPath = Place / PORTO_VOLUMES / Id / "volume";

    /* Verify volume path */
    if (!Path.IsEmpty()) {
        if (!Path.IsAbsolute())
            return TError(EError::InvalidValue, "Volume path must be absolute");
        if (!Path.IsNormal())
            return TError(EError::InvalidValue, "Volume path must be normalized");
        Path = CL->ResolvePath(Path);
        if (!Path.Exists())
            return TError(EError::InvalidValue, "Volume path does not exist");
        if (!Path.IsDirectoryStrict())
            return TError(EError::InvalidValue, "Volume path must be a directory");
        if (IsSystemPath(Path))
            return TError(EError::InvalidValue, "Volume in system directory");
    } else {
        if (CL->ClientContainer->RootPath.IsRoot()) {
            /* /place/porto_volumes/<id>/volume */
            Path = InternalPath;
        } else {
            /* /chroot/porto/volume_<id> */
            TPath porto_path = CL->ResolvePath(PORTO_CHROOT_VOLUMES);
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
    if (UserStorage()) {
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

    for (auto &it: Volumes) {
        auto other = it.second;
        if (StoragePath == other->StoragePath && Place == other->Place &&
                (!IsReadOnly || !other->IsReadOnly) &&
                (BackendType != "bind" || other->BackendType != "bind"))
            return TError(EError::Busy, "Storage already in use by volume " +
                                        other->Path.ToString());
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
            return TError(EError::LayerNotFound, "Layer not found");
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

    return TError::Success();
}

TError TVolume::Build() {
    auto lock = ScopedLock();
    TFile PathFd;

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
        error = CL->WriteAccess(StorageFd);
        if (error)
            return TError(error, "Volume " + Path.ToString());
    } else if (HaveStorage()) {
        TStorage storage(Place, PORTO_STORAGE, Storage);
        error = storage.Load();
        if (error)
            return error;
        error = CL->CanControl(storage.Owner);
        if (error)
            return TError(error, "Storage " + Storage);
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

    /* Create and pin volume path */
    if (IsAutoPath) {
        error = Path.Mkdir(0755);
        if (error)
            return error;
    }

    error = PathFd.OpenDir(Path);
    if (error)
        return error;

    TPath RealPath = PathFd.RealPath();
    if (RealPath != Path)
        return TError(EError::InvalidValue, "Volume real path differs: " +
                      RealPath.ToString() + " != " + Path.ToString());

    if (!IsAutoPath) {
        error = CL->WriteAccess(PathFd);
        if (error)
            return TError(error, "Volume " + Path.ToString());
    }

    if (Path != InternalPath) {
        error = InternalPath.Mkdir(0755);
        if (error)
            return error;
    }

    error = Backend->Build();
    if (error)
        return error;

    error = Backend->Save();
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
                    return TError(error, "Layer " + name);

                temp = GetInternal("temp");
                error = temp.Mkdir(0700);
                if (!error)
                    error = temp.BindRemount(pin.ProcPath(), MS_RDONLY | MS_NODEV);
                if (!error)
                    error = temp.Remount(MS_PRIVATE);
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

    /* And finally, publish volume in requested path */
    if (Path != InternalPath) {
        error = PathFd.ProcPath().Bind(InternalPath);
        if (error)
            return error;
    }

    PathFd.Close();
    StorageFd.Close();

    /* Keep storage only after successful build */
    if (!KeepStorage && HaveStorage())
        KeepStorage = true;

    IsReady = true;

    return Save();
}

void TVolume::DestroyAll() {
    std::list<std::shared_ptr<TVolume>> plan;
    for (auto &it : Volumes)
        plan.push_front(it.second);
    for (auto &vol: plan) {
        /* Volume can already be removed by parent */
        if (vol->IsDying)
            continue;
        TError error = vol->Destroy();
        if (error)
            L_WRN("Cannot destroy volume {} : {}", vol->Path , error);
    }
}

TError TVolume::Destroy(bool strict) {
    TError error, ret;

    std::list<std::shared_ptr<TVolume>> plan = {shared_from_this()};
    std::list<std::shared_ptr<TVolume>> work = {shared_from_this()};

    auto volumes_lock = LockVolumes();

    auto cycle = shared_from_this();

    for (auto it = work.begin(); it != work.end(); ++it) {
        auto &v = (*it);

        if (Path != v->Path) {
            while (!v->IsReady)
                VolumesCv.wait(volumes_lock);
        }

        v->IsDying = true;

        for (auto &nested : v->Nested) {
            if (strict)
                return TError(EError::Busy, "Volume "  + v->Path.ToString() + " depends on this");
            if (nested == cycle) {
                L_WRN("Cyclic dependencies for {} detected", cycle->Path);
            } else {
                auto plan_idx = std::find(plan.begin(), plan.end(), nested);

                if (plan_idx == plan.end())
                    cycle = nested;
                else
                    plan_idx = plan.erase(plan_idx);

                plan.push_front(nested);

                work.push_back(nested);
            }
        }
    }

    for (auto &volume : plan) {
        volumes_lock.unlock();
        error = volume->DestroyOne(strict);

        if (error && strict)
            return error;

        if (error && !ret)
            ret = error;

        volumes_lock.lock();

        for (auto &it : Volumes)
            it.second->Nested.erase(volume);

        Volumes.erase(volume->Path);

        for (auto &name: volume->Containers) {
            L_ACT("Forced unlink volume {} from {}", volume->Path, name);
            volumes_lock.unlock();
            auto containers_lock = LockContainers();
            auto container = TContainer::Find(name);
            containers_lock.unlock();
            volumes_lock.lock();
            if (container) {
                auto vol_iter = std::find(container->Volumes.begin(),
                                          container->Volumes.end(),
                                          volume);
                if (vol_iter != container->Volumes.end())
                    container->Volumes.erase(vol_iter);
            }
        }

        volumes_lock.unlock();

        for (auto &layer: volume->Layers) {
            TStorage storage(volume->Place, PORTO_LAYERS, layer);
            if (StringStartsWith(layer, PORTO_WEAK_PREFIX)) {
                error = storage.Remove();
                if (error && error.GetError() != EError::Busy)
                    L_ERR("Cannot remove weak layer {} : {}", layer, error);
            } else if (layer[0] != '/')
                (void)storage.Touch();
        }

        volumes_lock.lock();
    }

    VolumesCv.notify_all();

    return ret;
}

TError TVolume::DestroyOne(bool strict) {
    L_ACT("Destroy volume: {} backend: {}", Path, BackendType);

    auto lock = ScopedLock();

    TPath internal = GetInternal("");
    TError ret, error;

    if (strict && BackendType != "quota") {
        error = Path.Umount(UMOUNT_NOFOLLOW);
        if (error)
            return error;
    }

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
                L_ERR("Cannot remove storage: {}", error);

                error = StoragePath.RemoveAll();
                if (error) {
                    L_ERR("Cannot clear storage: {}", error);
                    if (!ret)
                        ret = error;
                }
            }
        } else {
            /* File image storage for backend=loop always persistent. */
            error = ClearRecursive(StoragePath);
            if (error)
                L_ERR("Cannot clear storage: {}", error);
            error = StoragePath.ClearDirectory();
            if (error) {
                L_ERR("Cannot clear storage: {}", error);
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
    error = node.Unlink();
    if (!ret && error)
        ret = error;

    lock.unlock();

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

        L_ACT("Resize volume: {} to bytes: {} inodes: {}",
                Path, spaceLimit, inodeLimit);

        error = Backend->Resize(spaceLimit, inodeLimit);
        if (error)
            return error;

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

    Containers.push_back(container.Name);
    TError error = Save();
    if (error)
        Containers.pop_back();
    else
        container.Volumes.emplace_back(shared_from_this());
    return error;
}

TError TVolume::UnlinkContainer(TContainer &container, bool strict) {
    auto volumes_lock = LockVolumes();

    auto it = std::find(container.Volumes.begin(),
                        container.Volumes.end(), shared_from_this());
    if (it == container.Volumes.end() && !Containers.empty())
        return TError(EError::VolumeNotLinked, "Container is not linked");

    if (strict && Containers.size() > 1)
        return TError(EError::Busy, "More than one linked container");

    if (IsDying)
        return TError(EError::Busy, "Volume is dying");

    container.Volumes.erase(it);
    Containers.erase(std::remove(Containers.begin(), Containers.end(),
                                 container.Name), Containers.end());
    if (Containers.empty())
        IsDying = true;

    return Save();
}

TStringMap TVolume::DumpConfig(const TPath &root) {
    TStringMap ret;
    TStatFS stat;

    if (IsReady && !StatFS(stat)) {
        ret[V_SPACE_USED] = std::to_string(stat.SpaceUsage);
        ret[V_INODE_USED] = std::to_string(stat.InodeUsage);
        ret[V_SPACE_AVAILABLE] = std::to_string(stat.SpaceAvail);
        ret[V_INODE_AVAILABLE] = std::to_string(stat.InodeAvail);
    }

    auto lock = LockVolumes();

    if (UserStorage() && !RemoteStorage())
        ret[V_STORAGE] = root.InnerPath(StoragePath).ToString();
    else
        ret[V_STORAGE] = Storage;

    ret[V_BACKEND] = BackendType;
    ret[V_OWNER_USER] = VolumeOwner.User();
    ret[V_OWNER_GROUP] = VolumeOwner.Group();
    if (VolumeCred.Uid != NoUser)
        ret[V_USER] = VolumeCred.User();
    if (VolumeCred.Gid != NoGroup)
        ret[V_GROUP] = VolumeCred.Group();
    if (VolumePerms)
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
    node.Set(V_OWNER_USER, VolumeOwner.User());
    node.Set(V_OWNER_GROUP, VolumeOwner.Group());
    if (VolumeCred.Uid != NoUser)
        node.Set(V_USER, VolumeCred.User());
    if (VolumeCred.Gid != NoGroup)
        node.Set(V_GROUP, VolumeCred.Group());
    if (VolumePerms)
        node.Set(V_PERMISSIONS, StringFormat("%#o", VolumePerms));
    node.Set(V_CREATOR, Creator);
    node.Set(V_READY, BoolToString(IsReady));
    node.Set(V_PRIVATE, Private);
    node.Set(V_RAW_CONTAINERS, MergeEscapeStrings(Containers, ';'));
    node.Set(V_LOOP_DEV, std::to_string(Device));
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

    TError error = ApplyConfig(node.Data);
    if (error)
        return error;

    InternalPath = Place / PORTO_VOLUMES / Id / "volume";

    if (!node.Has(V_OWNER_USER)) {
        auto tuple = SplitEscapedString(Creator, ' ');
        if (tuple.size() != 3 ||
                UserId(tuple[1], VolumeOwner.Uid) ||
                GroupId(tuple[2], VolumeOwner.Gid))
            VolumeOwner = VolumeCred;
    }

    if (!HaveStorage())
        StoragePath = GetInternal(BackendType);
    else if (!UserStorage() && !RemoteStorage())
        StoragePath = TStorage(Place, PORTO_STORAGE, Storage).Path;

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
    { V_BACKEND,     "plain|bind|tmpfs|quota|native|overlay|squash|loop|rbd (default - autodetect)", false },
    { V_STORAGE,     "path to data storage (default - internal)", false },
    { V_READY,       "true|false - contruction complete (ro)", true },
    { V_PRIVATE,     "user-defined property", false },
    { V_OWNER_USER,  "owner user (default - creator)", false },
    { V_OWNER_GROUP, "owner group (default - creator)", false },
    { V_USER,        "directory user (default - creator)", false },
    { V_GROUP,       "directory group (default - creator)", false },
    { V_PERMISSIONS, "directory permissions (default - 0775)", false },
    { V_CREATOR,     "container user group (ro)", true },
    { V_READ_ONLY,   "true|false (default - false)", false },
    { V_CONTAINERS,  "container;... - initial links, default - self", false },
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

TError TVolume::Create(const TStringMap &cfg, std::shared_ptr<TVolume> &volume) {
    TError error;

    if (!CL)
        return TError(EError::Unknown, "no client");

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

    auto volumes_lock = LockVolumes();

    auto max = config().volumes().max_total();
    if (Volumes.size() >= max)
        return TError(EError::ResourceNotAvailable, "number of volumes reached limit: " + std::to_string(max));

    if (cfg.count(V_PATH)) {
        TPath path(cfg.at(V_PATH));

        for (auto &it : Volumes) {
            auto &vol = it.second;

            if (vol->Path == path)
                return TError(EError::VolumeAlreadyExists, "Volume already exists");

            if (vol->Path.IsInside(path))
                return TError(EError::Busy, "Path overlaps with volume " + vol->Path.ToString());

            if (vol->Place.IsInside(path))
                return TError(EError::Busy, "Path overlaps with place " + vol->Place.ToString());

            if (vol->StoragePath.IsInside(path) || path.IsInside(vol->StoragePath))
                return TError(EError::Busy, "Path overlaps with storage " + vol->StoragePath.ToString());

            for (auto &l: vol->Layers) {
                TPath layer(l);
                if (layer.IsAbsolute() && (layer.IsInside(path) || path.IsInside(layer)))
                    return TError(EError::Busy, "Path overlaps with layer " + l);
            }
        }
    }

    volume = std::make_shared<TVolume>();
    volume->Id = std::to_string(NextId++);

    error = volume->Configure(cfg);
    if (error)
        return error;

    error = volume->CheckDependencies();
    if (error)
        return error;

    Volumes[volume->Path] = volume;

    volumes_lock.unlock();

    if (cfg.count(V_CONTAINERS)) {
        for (auto &name: SplitEscapedString(cfg.at(V_CONTAINERS), ';')) {
            std::shared_ptr<TContainer> ct;
            error = CL->WriteContainer(name, ct, true);
            if (!error)
                error = volume->LinkContainer(*ct);
            if (error) {
                volume->Destroy();
                return error;
            }
        }
    } else {
        error = volume->LinkContainer(*CL->ClientContainer);
        if (error) {
            volume->Destroy();
            return error;
        }
    }

    error = volume->Build();
    if (error) {
        (void)volume->Destroy();
        return error;
    }

    VolumesCv.notify_all();

    return TError::Success();
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
        node.Name = node.Get(V_ID);
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

        auto containers_lock = LockContainers();

        for (auto &name: volume->Containers) {
            auto container = TContainer::Find(name);
            if (container)
                container->Volumes.emplace_back(volume);
            else
                L_WRN("Cannot find container {}", name);
        }

        containers_lock.unlock();

        error = volume->Save();
        if (error) {
            broken_volumes.push_back(volume);
            continue;
        }

        if (!volume->Containers.size()) {
            L_WRN("Volume {} has no containers", volume->Path);
            broken_volumes.push_back(volume);
            continue;
        }

        error = volume->CheckDependencies();
        if (error) {
            L_WRN("Volume {} has broken dependcies: {}", volume->Path, error);
            broken_volumes.push_back(volume);
            continue;
        }

        L("Volume {} restored", volume->Path);
    }

    L_ACT("Remove broken volumes...");

    for (auto &volume : broken_volumes) {
        if (volume->IsDying)
            continue;

        (void)volume->Destroy();
    }

    TPath volumes = place / PORTO_VOLUMES;

    L_ACT("Remove stale volumes...");

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
            L_ERR("Cannot remove directory {}: {}", dir, error);

            error = dir.RemoveAll();
            if (error)
                L_ERR("Cannot delete directory {}", dir);
        }
    }

    L_ACT("Remove stale weak layers...");

    std::list<TStorage> layers;

    error = TStorage::List(place, PORTO_LAYERS, layers);
    if (!error) {
        for (auto &layer : layers) {
            if (StringStartsWith(layer.Name, PORTO_WEAK_PREFIX)) {

                error = layer.Remove();
                if (error && error.GetError() != EError::Busy)
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
            if (prop.second == "true")
                IsAutoPath = true;
            else if (prop.second == "false")
                IsAutoPath = false;
            else
                return TError(EError::InvalidValue, "Invalid bool value");

        } else if (prop.first == V_STORAGE) {
            Storage = prop.second;
            StoragePath = Storage;
            KeepStorage = HaveStorage();

        } else if (prop.first == V_BACKEND) {
            BackendType = prop.second;

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

        } else if (prop.first == V_ID) {
            Id = prop.second;

        } else if (prop.first == V_READY) {
            error = StringToBool(prop.second, IsReady);
            if (error)
                return error;

        } else if (prop.first == V_PRIVATE) {
            Private = prop.second;

        } else if (prop.first == V_RAW_CONTAINERS) {
            Containers = SplitEscapedString(prop.second, ';');

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

        } else {
            return TError(EError::InvalidValue, "Invalid value name: " + prop.first);
        }
    }

    return TError::Success();
}
