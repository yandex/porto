#include <sstream>
#include <algorithm>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <thread>

#include "volume.hpp"
#include "storage.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/quota.hpp"
#include "util/thread.hpp"
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
MeasuredMutex VolumesMutex("volumes");
std::map<TPath, std::shared_ptr<TVolume>> Volumes;
std::map<TPath, std::shared_ptr<TVolumeLink>> VolumeLinks;
static std::atomic<uint64_t> NextId(1);

static std::condition_variable VolumesCv;

std::unique_ptr<std::thread> StatFsThread;
bool NeedStopStatFsLoop(false);
std::condition_variable StatFsCv;
std::mutex StatFsLock;

void SleepStatFsLoop(std::unique_lock<std::mutex>& locker, const uint64_t sleepTime) {
    auto now = GetCurrentTimeMs();
    auto deadline = now + sleepTime;

    while (!NeedStopStatFsLoop && deadline > now) {
        StatFsCv.wait_for(locker, std::chrono::milliseconds(deadline - now));
        now = GetCurrentTimeMs();
    }
}

void StatFsUpdateLoop() {
    SetProcessName("portod-FS");

    const uint64_t statFsUpdateInterval = config().volumes().fs_stat_update_interval_ms();
    std::unique_lock<std::mutex> locker(StatFsLock);

    while (!NeedStopStatFsLoop) {
        std::list<std::shared_ptr<TVolume>> volumes;
        locker.unlock();
        auto volumes_lock = LockVolumes();

        for (auto& it: Volumes)
            volumes.push_back(it.second);

        volumes_lock.unlock();
        locker.lock();

        const uint64_t sleepTime = volumes.empty() ? statFsUpdateInterval : statFsUpdateInterval / volumes.size();

        if (volumes.empty()) {
            SleepStatFsLoop(locker, sleepTime);
            continue;
        }

        for (auto& volume: volumes) {
            if (NeedStopStatFsLoop)
                return;

            locker.unlock();
            volume->UpdateStatFS();
            locker.lock();

            SleepStatFsLoop(locker, sleepTime);
        }
    }
}

void StartStatFsLoop() {
    NeedStopStatFsLoop = false;
    StatFsThread = std::unique_ptr<std::thread>(NewThread(&StatFsUpdateLoop));
}

void StopStatFsLoop() {
    {
        std::unique_lock<std::mutex> locker(StatFsLock);
        NeedStopStatFsLoop = true;
    }
    StatFsCv.notify_all();
    StatFsThread->join();
}

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

TError TVolumeBackend::Check() {
    return OK;
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

        if (Volume->BackendType == "hugetmpfs" &&
                !TPath("/sys/kernel/mm/transparent_hugepage/shmem_enabled").Exists())
            return TError(EError::NotSupported, "kernel does not support transparent huge pages in tmpfs");

        if (!Volume->SpaceLimit)
            return TError(EError::InvalidProperty, "tmpfs backend requires space_limit");

        if (Volume->HaveStorage())
            return TError(EError::InvalidProperty, "tmpfs backed doesn't support storage");

        if (Volume->HaveLayers())
            return TError(EError::InvalidProperty, "tmpfs backed doesn't support layers");

        return OK;
    }

    TError Build() override {
        std::vector<std::string> opts;

        if (Volume->BackendType == "hugetmpfs")
            opts.emplace_back("huge=always");

        if (Volume->SpaceLimit)
            opts.emplace_back("size=" + std::to_string(Volume->SpaceLimit));

        if (Volume->InodeLimit)
            opts.emplace_back("nr_inodes=" + std::to_string(Volume->InodeLimit));

        return Volume->InternalPath.Mount("porto_tmpfs_" + Volume->Id, "tmpfs",
                                          Volume->GetMountFlags(), opts);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        std::vector<std::string> opts;

        if (Volume->BackendType == "hugetmpfs")
            opts.emplace_back("huge=always");

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

    TError Check() override {
        return TProjectQuota(Volume->Path).Check();
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

    TError Check() override {
        TProjectQuota quota(Volume->Path);

        if (Volume->HaveQuota() && quota.Exists())
            return quota.Check();
        else
            return TError(EError::NotSupported, "Volume has no quota or project doesn't exist");
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
        L("Cannot enable O_DIRECT for loop {}", TError::System("fcntl"));

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

    TPath scheduler(fmt::format("/sys/block/loop{}/queue/scheduler", nr));
    error = scheduler.WriteAll("none");
    if (error)
        return error;

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
        if (Volume->DeviceIndex < 0)
            return TPath();
        return TPath("/dev/loop" + std::to_string(Volume->DeviceIndex));
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

        /* needs CAP_SYS_RESOURCE */
        return RunCommand({"resize2fs", path, size}, TFile(), TFile(), TFile(), HostCapBound);
    }

    TError Build() override {
        TPath path = ImagePath(Volume->StoragePath);
        struct stat st;
        TError error;
        TFile file;
        bool file_storage = false;

        if (Volume->StorageFd.IsRegular()) {
            error = file.Dup(Volume->StorageFd);
            file_storage = true;
        } else if (Volume->StorageFd.ExistsAt(AutoImage)) {
            error = file.OpenAt(Volume->StorageFd, AutoImage,
                                (Volume->IsReadOnly ? O_RDONLY : O_RDWR) |
                                O_CLOEXEC | O_NOCTTY | O_NOFOLLOW, 0);
        } else {
            error = file.OpenAt(Volume->StorageFd, AutoImage,
                                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (!error) {
                Volume->KeepStorage = false; /* New storage */
                error = MakeImage(file, Volume->StorageFd, path, Volume->SpaceLimit, Volume->SpaceGuarantee);
                if (error)
                    Volume->StorageFd.UnlinkAt(AutoImage);
            }
        }
        if (!error)
            error = file.Stat(st);
        if (error)
            return error;
        if (!S_ISREG(st.st_mode))
            return TError(EError::InvalidData, "loop image should be a regular file");

        /* Protect image from concurrent changes */
        if (st.st_uid != RootUser || st.st_gid != RootGroup || (st.st_mode & 2)) {
            error = file.Chown(RootUser, RootGroup);
            if (error)
                return error;
            error = file.Chmod(0644);
            if (error)
                return error;
        }

        if (!Volume->SpaceLimit) {
            Volume->SpaceLimit = st.st_size;
        } else if (!Volume->IsReadOnly && (uint64_t)st.st_size != Volume->SpaceLimit) {
            if (file_storage) {
                error = ResizeImage(file, TFile(),
                        path, st.st_size, Volume->SpaceLimit);
            } else {
                error = ResizeImage(file, Volume->StorageFd,
                        path, st.st_size, Volume->SpaceLimit);
            }
            if (error)
                return error;
        }

        error = SetupLoopDev(file, path, Volume->DeviceIndex);
        if (error)
            return error;

        error = Volume->InternalPath.Mount(GetLoopDevice(), "ext4",
                                           Volume->GetMountFlags(), {});
        if (error) {
            PutLoopDev(Volume->DeviceIndex);
            Volume->DeviceIndex = -1;
        }

        return error;
    }

    TError Destroy() override {
        TPath loop = GetLoopDevice();

        if (Volume->DeviceIndex < 0)
            return OK;

        L_ACT("Destroy loop {}", loop);
        TError error = Volume->InternalPath.UmountAll();
        TError error2 = PutLoopDev(Volume->DeviceIndex);
        if (!error)
            error = error2;
        Volume->DeviceIndex = -1;
        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        if (Volume->IsReadOnly)
            return TError(EError::Busy, "Volume is read-only");
        if (Volume->SpaceLimit < (512ul << 20))
            return TError(EError::InvalidProperty, "Refusing to online resize loop volume with initial limit < 512M (kernel bug)");

        (void)inode_limit;
        return ResizeLoopDev(Volume->DeviceIndex, ImagePath(Volume->StoragePath),
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
        TFile upperFd, workFd, cowFd;
        struct stat st;
        std::unordered_map<dev_t, std::unordered_set<ino_t> > ovlInodes;

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
                    if (config().volumes().insecure_user_paths())
                        L("Ignore {}", error);
                    else
                        goto err;
                }
                path = pin.ProcPath();
            } else {
                TStorage layer;
                layer.Open(EStorageType::Layer, Volume->Place, name);
                /* Imported layers are available for everybody */
                (void)layer.Touch();
                path = layer.Path;

                error = pin.OpenDir(path);
                if (error) {
                    error = TError(error, "Cannot open layer {} in place {}", name, Volume->Place);
                    goto err;
                }
            }

            error = pin.Stat(st);
            if (error)
                goto err;

            auto dev_inode = ovlInodes.find(st.st_dev);
            if (dev_inode != ovlInodes.end()) {
                if (!dev_inode->second.emplace(st.st_ino).second) {
                    pin.Close();
                    L("Skipping duplicate lower layer {}", name);
                    continue;
                }
            } else {
                ovlInodes.emplace(st.st_dev, std::unordered_set<ino_t>()).first->second.emplace(st.st_ino);
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

        error = upperFd.OpenDirStrictAt(Volume->StorageFd, "upper");
        if (error)
            goto err;

        (void)Volume->StorageFd.MkdirAt("work", 0755);

        error = workFd.OpenDirStrictAt(Volume->StorageFd, "work");
        if (error)
            goto err;

        error = workFd.ClearDirectory();
        if (error)
            goto err;

        error = Volume->GetInternal("").Chdir();
        if (error)
            goto err;

        error = Volume->MakeDirectories(upperFd);
        if (error)
            goto err;

        error = Volume->MakeSymlinks(upperFd);
        if (error)
            goto err;

        error = Volume->MakeShares(upperFd, false);
        if (error)
            goto err;

        if (Volume->NeedCow) {
            error = cowFd.CreateDirAllAt(Volume->StorageFd, "cow", 0755, Volume->VolumeCred);
            if (error)
                goto err;

            error = Volume->MakeShares(cowFd, true);
            if (error)
                goto err;
        } else
            (void)cowFd.OpenDirAt(Volume->StorageFd, "cow");

        if (cowFd)
            lower = cowFd.ProcPath().ToString() + ":" + lower;

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
            TPath temp = Volume->GetInternal("L" + std::to_string(Volume->Layers.size() - layer_idx - 1));
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

    TError Check() override {
        TProjectQuota quota(Volume->StoragePath);

        if (Volume->HaveQuota() && quota.Exists())
            return quota.Check();
        else
            return TError(EError::NotSupported, "Volume has no quota or project doesn't exist");
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
        TFile lowerFd, upperFd, workFd, cowFd;
        std::string lowerdir;
        int layer_idx = 0;
        TError error;
        TPath lower;
        struct stat st;
        std::unordered_map<dev_t, std::unordered_set<ino_t> > ovlInodes;

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

        error = upperFd.OpenDirStrictAt(Volume->StorageFd, "upper");
        if (error)
            return error;

        (void)Volume->StorageFd.MkdirAt("work", 0755);

        error = workFd.OpenDirStrictAt(Volume->StorageFd, "work");
        if (error)
            return error;

        error = workFd.ClearDirectory();
        if (error)
            return error;

        error = SetupLoopDev(lowerFd, Volume->Layers[0], Volume->DeviceIndex);
        if (error)
            return error;

        error = lower.Mount("/dev/loop" + std::to_string(Volume->DeviceIndex),
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
                    if (config().volumes().insecure_user_paths())
                        L("Ignore {}", error);
                    else
                        goto err;
                }
                path = pin.ProcPath();
            } else {
                TStorage layer;
                layer.Open(EStorageType::Layer, Volume->Place, name);
                /* Imported layers are available for everybody */
                (void)layer.Touch();
                path = layer.Path;

                error = pin.OpenDir(path);
                if (error) {
                    error = TError(error, "Cannot open layer {} in place {}", name, Volume->Place);
                    goto err;
                }
            }

            error = pin.Stat(st);
            if (error)
                goto err;

            auto dev_inode = ovlInodes.find(st.st_dev);
            if (dev_inode != ovlInodes.end()) {
                if (!dev_inode->second.emplace(st.st_ino).second) {
                    pin.Close();
                    L("Skipping duplicate lower layer {}", name);
                    continue;
                }
            } else {
                ovlInodes.emplace(st.st_dev, std::unordered_set<ino_t>()).first->second.emplace(st.st_ino);
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

        error = Volume->MakeDirectories(upperFd);
        if (error)
            goto err;

        error = Volume->MakeSymlinks(upperFd);
        if (error)
            goto err;

        error = Volume->MakeShares(upperFd, false);
        if (error)
            goto err;

        if (Volume->NeedCow) {
            error = cowFd.CreateDirAllAt(Volume->StorageFd, "cow", 0755, Volume->VolumeCred);
            if (error)
                goto err;

            error = Volume->MakeShares(cowFd, true);
            if (error)
                goto err;
        } else
            (void)cowFd.OpenDirAt(Volume->StorageFd, "cow");

        if (cowFd)
            lowerdir = cowFd.ProcPath().ToString() + ":" + lowerdir;

        error = Volume->InternalPath.Mount("overlay", "overlay",
                                   Volume->GetMountFlags(),
                                   { "lowerdir=" + lowerdir,
                                     "upperdir=" + upperFd.ProcPath().ToString(),
                                     "workdir=" + workFd.ProcPath().ToString() });

        if (error && error.Errno == EINVAL && Volume->Layers.size() >= 500)
            error = TError(EError::InvalidValue, "Too many layers, kernel limits is 499 plus 1 for upper");

        while (layer_idx--) {
            TPath temp = Volume->GetInternal("L" + std::to_string(Volume->Layers.size() - layer_idx - 2));
            (void)temp.UmountAll();
            (void)temp.Rmdir();
        }

err:
        (void)TPath("/").Chdir();

        if (error) {
            if (Volume->HaveQuota())
                (void)quota.Destroy();
            if (Volume->DeviceIndex >= 0) {
                lower.UmountAll();
                PutLoopDev(Volume->DeviceIndex);
                Volume->DeviceIndex = -1;
            }
        }

        return error;
    }

    TError Destroy() override {
        if (Volume->DeviceIndex >= 0) {
            Volume->InternalPath.UmountAll();
            Volume->GetInternal("lower").UmountAll();
            PutLoopDev(Volume->DeviceIndex);
            Volume->DeviceIndex = -1;
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
        if (Volume->DeviceIndex < 0)
            return "";
        return "/dev/rbd" + std::to_string(Volume->DeviceIndex);
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

        error = StringToInt(device.substr(8), Volume->DeviceIndex);
        if (error) {
            UnmapDevice(device);
            return error;
        }

        Volume->DeviceName = fmt::format("rbd{}", Volume->DeviceIndex);

        error = Volume->InternalPath.Mount(device, "ext4",
                                           Volume->GetMountFlags(), {});
        if (error)
            UnmapDevice(device);
        return error;
    }

    TError Destroy() override {
        std::string device = GetDevice();
        TError error, error2;

        if (Volume->DeviceIndex < 0)
            return OK;

        error = Volume->InternalPath.UmountAll();
        error2 = UnmapDevice(device);
        if (!error)
            error = error2;
        Volume->DeviceIndex = -1;
        return error;
    }

    TError Resize(uint64_t, uint64_t) override {
        return TError(EError::NotSupported, "rbd backend doesn't suppport resize");
    }

    std::string ClaimPlace() override {
        return "rbd";
    }

    TError Restore() {
        if (!Volume->DeviceName.size())
            Volume->DeviceName = fmt::format("rbd{}", Volume->DeviceIndex);
        return OK;
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
        TError error;

        error = Configure();
        if (error)
            return error;

        if (!Volume->DeviceName.size())
            TPath::GetDevName(TPath(Device).GetBlockDev(), Volume->DeviceName);

        return OK;
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

        TPath::GetDevName(TPath(Device).GetBlockDev(), Volume->DeviceName);

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
        /* needs CAP_SYS_RESOURCE */
        return RunCommand({"lvm", "lvresize", "--force", "--resizefs",
                           "--size", std::to_string(space_limit) + "B",
                           Device}, TFile(), TFile(), TFile(), HostCapBound);
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

TPath TVolume::ComposePathLocked(const TContainer &ct) const {
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

TPath TVolume::ComposePath(const TContainer &ct) const {
    auto volumes_lock = LockVolumes();
    return ComposePathLocked(ct);
}

std::string TVolume::StateName(EVolumeState state) {
    switch (state) {
    case EVolumeState::Initial:
        return "initial";
    case EVolumeState::Building:
        return "building";
    case EVolumeState::Ready:
        return "ready";
    case EVolumeState::Tuning:
        return "tuning";
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
    else if (BackendType == "tmpfs" || BackendType == "hugetmpfs")
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

/* create and remove file on volume to load quota file into cache */
void TVolume::CacheQuotaFile() {
    TFile tmp;
    TPath tmp_path = Path + "/" + PORTO_CACHE_QUOTA_FILE_NAME;
    TError err = tmp.CreateNew(tmp_path, S_IRUSR | S_IWUSR);
    if (err) {
        L_WRN("Failed to load quota file into cache for volume \"{}\" : {}", Path, err);
        return;
    }

    tmp.Close();
    err = tmp_path.Unlink();
    if (err)
        L_WRN("Failed to unlink tmp file \"{}\" on volume \"{}\" created to load quota file into cache : {}", tmp_path, Path, err);

    return;
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

    /* in host namespace volumes are nodev and nosuid */
    flags |= MS_NODEV | MS_NOSUID;

    return flags;
}

/* Called under VolumesMutex */
TError TVolume::CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee) {
    TStatFS current, total;
    TPath storage;
    TError error;

    if (RemoteStorage() || (!space_guarantee && !inode_guarantee))
        return OK;

    if (UserStorage())
        storage = StoragePath;
    else if (HaveStorage()) {
        TStorage back;
        back.Open(EStorageType::Storage, Place, Storage);
        storage = back.Path.DirName();
    } else
        storage = Place / PORTO_VOLUMES;

    error = storage.StatFS(total);
    if (error)
        return error;

    StatFS(current);

    /* Check available space as is */
    if (total.SpaceAvail + current.SpaceUsage < space_guarantee)
        return TError(EError::NoSpace, "Not enough space for volume guarantee {}, avail {}, our usage {}",
                      StringFormatSize(space_guarantee),
                      StringFormatSize(total.SpaceAvail),
                      StringFormatSize(current.SpaceUsage));

    if (total.InodeAvail + current.InodeUsage < inode_guarantee &&
            BackendType != "loop")
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee {}, avail {}, our usage {}",
                      inode_guarantee, total.InodeAvail, current.InodeUsage);

    /* Estimate unclaimed guarantees */
    uint64_t space_claimed = 0, space_guaranteed = 0;
    uint64_t inode_claimed = 0, inode_guaranteed = 0;

    for (auto &it: Volumes) {
        auto volume = it.second;

        /* data stored remotely, plain cannot provide usage */
        if (volume.get() == this ||
                volume->RemoteStorage() ||
                volume->BackendType == "plain" ||
                volume->StoragePath.GetDev() != storage.GetDev() ||
                (!volume->SpaceGuarantee && !volume->InodeGuarantee))
            continue;

        TStatFS stat;
        volume->StatFS(stat);

        space_guaranteed += volume->SpaceGuarantee;
        space_claimed += std::min(stat.SpaceUsage, volume->SpaceGuarantee);

        if (volume->BackendType != "loop") {
            inode_guaranteed += volume->InodeGuarantee;
            inode_claimed += std::min(stat.InodeUsage, volume->InodeGuarantee);
        }
    }

    if (total.SpaceAvail + current.SpaceUsage + space_claimed <
            space_guarantee + space_guaranteed)
        return TError(EError::NoSpace, "Not enough space for volume guarantee {}, avail {}, claimed {} of {}, our usage {}",
                      StringFormatSize(space_guarantee),
                      StringFormatSize(total.SpaceAvail),
                      StringFormatSize(space_claimed),
                      StringFormatSize(space_guaranteed),
                      StringFormatSize(current.SpaceUsage));

    if (BackendType != "loop" &&
            total.InodeAvail + current.InodeUsage + inode_claimed <
            inode_guarantee + inode_guaranteed)
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee {}, avail {}, claimed {} of {}, our usage {}",
                      inode_guarantee, total.InodeAvail, inode_claimed, inode_guaranteed, current.InodeUsage);

    return OK;
}

TError TVolume::ClaimPlace(uint64_t size) {

    if (!VolumeOwnerContainer || !Backend)
        return TError(EError::Busy, "Volume has no backend or owner");

    auto place = Backend->ClaimPlace();
    if (place == "")
        return OK;

    for (auto ct = VolumeOwnerContainer; ct; ct = ct->Parent) {
        ct->LockStateWrite();

        if ((!size || size > ClaimedSpace) && !CL->IsInternalUser() &&
                State != EVolumeState::Destroying) {

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
                    (place_limit < place_usage - ClaimedSpace + size)) {

                ct->UnlockState();

                // Undo
                for (auto c = VolumeOwnerContainer; c && c != ct; c = c->Parent) {
                    c->LockStateWrite();
                    c->PlaceUsage["total"] -= size - ClaimedSpace;
                    c->PlaceUsage[place] -= size - ClaimedSpace;
                    c->UnlockState();
                }

                return TError(EError::ResourceNotAvailable,
                              "Not enough place limit in {} for {} limit {}, total usage {} of {}, {} usage {} of {}",
                              ct->Name, Path, StringFormatSize(size - ClaimedSpace),
                              StringFormatSize(total_usage), StringFormatSize(total_limit),
                              place, StringFormatSize(place_usage), StringFormatSize(place_limit));
            }
        }

        ct->PlaceUsage["total"] += size - ClaimedSpace;
        ct->PlaceUsage[place] += size - ClaimedSpace;

        ct->UnlockState();
    }

    auto volumes_lock = LockVolumes();
    ClaimedSpace = size;

    return OK;
}

TError TVolume::DependsOn(const TPath &path) {
    if (State == EVolumeState::Ready && !path.Exists())
        return TError(EError::VolumeNotFound, "Volume {} depends on non-existent path {}", Path, path);

    auto link = ResolveOriginLocked(path);
    if (link) {
        if (link->Volume->State != EVolumeState::Ready &&
                link->Volume->State != EVolumeState::Tuning)
            return TError(EError::VolumeNotReady, "Volume {} depends on non-ready volume {}", Path, link->Volume->Path);
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
        if (!layer.IsAbsolute()) {
            TStorage layer_storage;
            layer_storage.Open(EStorageType::Layer, Place, l);
            layer = layer_storage.Path;
        }
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
        return TError(EError::InvalidPath, "Volume path {} in system directory", path);

    for (auto &it : Volumes) {
        auto &vol = it.second;

        if (vol->Path == path)
            return TError(EError::Busy, "Volume path {} is used by volume {}", path, vol->Path);

        if (vol->Path.IsInside(path))
            return TError(EError::InvalidPath, "Volume path {} overlaps with volume {}", path, vol->Path);

        if (path.IsInside(vol->Path) &&
                vol->State != EVolumeState::Ready &&
                vol->State != EVolumeState::Tuning)
            return TError(EError::VolumeNotReady, "Volume path {} inside volume {} and it is not ready", path, vol->Path);

        if (vol->Place.IsInside(path))
            return TError(EError::InvalidPath, "Volume path {} overlaps with place {}", path, vol->Place);

        if (vol->RemoteStorage()) {

        } else if (vol->BackendType == "bind" || vol->BackendType == "rbind") {
            if (vol->StoragePath.IsInside(path))
                return TError(EError::InvalidPath, "Volume path {} overlaps with volume {} storage {}", path, vol->Path, vol->StoragePath);
        } else {
            if (vol->StoragePath.IsInside(path) || path.IsInside(vol->StoragePath))
                return TError(EError::InvalidPath, "Volume path {} overlaps with volume {} storage {}", path, vol->Path, vol->StoragePath);
        }

        for (auto &l: vol->Layers) {
            TPath layer(l);
            if (layer.IsAbsolute() && (layer.IsInside(path) || path.IsInside(layer)))
                return TError(EError::InvalidPath, "Volume path {} overlaps with layer {}", path, layer);
        }

        for (auto &link: vol->Links) {
            if (link->HostTarget == path)
                return TError(EError::Busy, "Volume path {} is used by volume {} for {}", path, vol->Path, link->Container->Name);
            if (link->HostTarget.IsInside(path))
                return TError(EError::InvalidPath, "Volume path {} overlaps with volume {} link {} for {}", path, vol->Path, link->HostTarget, link->Container->Name);
        }
    }

    return OK;
}

TError TVolume::Configure(const TPath &target_root) {
    TError error;

    error = CL->ClientContainer->ResolvePlace(Place);
    if (error)
        return error;

    /* Verify credentials */
    error = CL->CanControl(VolumeOwner);
    if (error)
        return TError(error, "Volume {}", Path);

    if (VolumeOwner.GetGid() != CL->Cred.GetGid() && !CL->IsSuperUser() &&
            !CL->Cred.IsMemberOf(VolumeOwner.GetGid()))
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
            return TError(EError::InvalidPath, "Storage path must be normalized");
        StoragePath = CL->ResolvePath(StoragePath);
        if (!StoragePath.Exists())
            return TError(EError::InvalidPath, "Storage path does not exist");
        if (IsSystemPath(StoragePath))
            return TError(EError::InvalidPath, "Storage in system directory");
        Storage = StoragePath.ToString();
        KeepStorage = true;
    } else if (!HaveStorage()) {
        StoragePath = GetInternal(BackendType);
        KeepStorage = false;
    } else {
        error = TStorage::CheckName(Storage);
        if (error)
            return error;
        TStorage storage;
        storage.Open(EStorageType::Storage, Place, Storage);
        StoragePath = storage.Path;
        KeepStorage = storage.Exists();
    }

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
            return TError(EError::InvalidPath, "Layer path must be normalized");
        if (layer.IsAbsolute()) {
            layer = CL->ResolvePath(layer);
            l = layer.ToString();
        } else {
            error = TStorage::CheckName(l);
            if (error)
                return error;
            TStorage layer_storage;
            layer_storage.Open(EStorageType::Layer, Place, l);
            layer = layer_storage.Path;
        }
        if (!layer.Exists())
            return TError(EError::LayerNotFound, "Layer not found " + layer.ToString());
        if (IsSystemPath(layer))
            return TError(EError::InvalidPath, "Layer path {} in system directory", layer);
        if (!layer.IsDirectoryFollow() && BackendType != "squash")
            return TError(EError::InvalidPath, "Layer must be a directory");
        /* Permissions will be cheked during build */
    }

    if (HaveLayers() && BackendType != "overlay" && BackendType != "squash") {
        if (IsReadOnly)
            return TError(EError::InvalidValue, "Cannot copy layers to read-only volume");
    }

    for (auto &share: Spec->shares())
        NeedCow |= share.cow();

    if (NeedCow && BackendType != "overlay" && BackendType != "squash")
        return TError(EError::InvalidValue, "Backend {} does not support copy-on-write shares", BackendType);

    /* Verify guarantees */
    if (SpaceLimit && SpaceLimit < SpaceGuarantee)
        return TError(EError::InvalidValue, "Space guarantree bigger than limit");

    if (InodeLimit && InodeLimit < InodeGuarantee)
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

TError TVolume::MergeLayers() {
    TError error;

    if (!HaveLayers())
        return OK;

    for (auto &name : Layers) {
        L_ACT("Merge layer {} into volume: {}", name, Path);

        if (name[0] == '/') {
            TPath temp;
            TFile pin;

            error = pin.OpenDir(name);
            if (error)
                return error;

            error = CL->WriteAccess(pin);
            if (error) {
                error = TError(error, "Layer {}", name);
                if (config().volumes().insecure_user_paths())
                    L("Ignore {}", error);
                else
                    return error;
            }

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
            TStorage layer_storage;
            layer_storage.Open(EStorageType::Layer, Place, name);
            (void)layer_storage.Touch();
            /* Imported layers are available for everybody */
            error = CopyRecursive(layer_storage.Path, InternalPath);
        }
        if (error)
            return error;

        error = TStorage::SanitizeLayer(InternalPath);
        if (error)
            return error;
    }

    return OK;
}

TError TVolume::MakeDirectories(const TFile &base) {
    TError error;

    for (auto &dir_spec: Spec->directories()) {
        int perms = dir_spec.has_permissions() ? dir_spec.permissions() : VolumePermissions;
        TPath path = dir_spec.path();
        TCred cred = VolumeCred;
        TFile dir;

        if (path.IsAbsolute() || !path.IsNormal() || path.StartsWithDotDot())
            return TError(EError::InvalidPath, "directory path {}", path);

        if (dir_spec.has_cred()) {
            error = cred.Load(dir_spec.cred(), false);
            if (error)
                return error;
        }

        L("Make directory {}", path);

        error = dir.CreateDirAllAt(base, path, perms, cred);
        if (error)
            return error;
    }

    return OK;
}

TError TVolume::MakeSymlinks(const TFile &base) {
    TError error;

    for (auto &sym_spec: Spec->symlinks()) {
        TPath path = sym_spec.path();
        TPath target = sym_spec.target_path();
        TFile dir;

        if (path.IsAbsolute() || !path.IsNormal() || path.StartsWithDotDot())
            return TError(EError::InvalidPath, "symlink path {}", path);

        L("Make symlink {} -> {}", path, target);

        error = dir.CreateDirAllAt(base, path.DirName(), VolumePermissions, VolumeCred);
        if (error)
            return error;

        TPath base_name = path.BaseName();
        TPath old_target;

        if (!dir.ReadlinkAt(base_name, old_target))
            dir.UnlinkAt(base_name);

        error = dir.SymlinkAt(base_name, target);
        if (error)
            return error;
    }

    return OK;
}

TError TVolume::MakeShares(const TFile &base, bool cow) {
    TFile old_cwd, old_root, new_root;
    TError error, error2;

    error = old_cwd.OpenDir(".");
    if (error)
        return error;

    error = old_root.OpenDir("/");
    if (error)
        return error;

    for (auto &share: Spec->shares()) {

        if (share.cow() != cow)
            continue;

        TPath path = share.path();
        TPath origin_path = share.origin_path();

        if (path.IsAbsolute() || !path.IsNormal() || !path || path.StartsWithDotDot())
            return TError(EError::InvalidPath, "share path {}", path);

        if (!origin_path.IsAbsolute() || !origin_path.IsNormal())
            return TError(EError::InvalidPath, "share origin {}", origin_path);

        L("Make{} share {} -> {}", share.cow()? " cow" : "", path, origin_path);

        TPath root_path = CL->ClientContainer->RootPath;
        TFile new_root;
        if (!root_path.IsRoot()) {
            error = new_root.OpenDir(root_path);
            if (error)
                return error;
            error = new_root.Chroot();
            if (error)
                return error;
        }

        TFile src;
        CL->TaskCred.Enter();
        error = src.OpenRead(origin_path);
        CL->TaskCred.Leave();

        if (new_root) {
            error2 = old_root.Chroot();
            PORTO_ASSERT(!error2);
            error2 = old_cwd.Chdir();
            PORTO_ASSERT(!error2);
        }

        if (error)
            return error;

        if (!share.cow() || src.IsDirectory()) {
            error = CL->WriteAccess(src);
            if (error)
                return error;
        }

        // Open origin at base mount
        error = src.OpenAtMount(base, src, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (error)
            return error;

        TFile dir;
        error = dir.CreateDirAllAt(base, path.DirName(), VolumePermissions, VolumeCred);
        if (error)
            return error;

        if (src.IsDirectory()) {
            TPathWalk walk;

            error = src.Chdir();
            if (error)
                return error;

            error = walk.OpenList(".");
            while (!error) {
                error = walk.Next();
                if (error || !walk.Path)
                    break;
                TPath name = walk.Level() ? walk.Name() : path.BaseName();
                if (walk.Directory) {
                    if (walk.Postorder) {
                        error = dir.OpenDirStrictAt(dir, "..");
                        if (!error && walk.Level())
                            error = src.OpenDirStrictAt(src, "..");
                    } else {
                        if (!dir.MkdirAt(name, walk.Stat->st_mode & 07777))
                            error = dir.ChownAt(name, walk.Stat->st_uid, walk.Stat->st_gid);
                        if (!error)
                            error = dir.OpenDirStrictAt(dir, name);
                        if (!error && walk.Level())
                            error = src.OpenDirStrictAt(src, name);
                    }
                } else if (S_ISREG(walk.Stat->st_mode)) {
                    TProjectQuota::Toggle(dir, false);
                    (void)dir.UnlinkAt(name);
                    error = dir.HardlinkAt(name, src, walk.Name());
                    TProjectQuota::Toggle(dir, true);
                } else if (S_ISLNK(walk.Stat->st_mode)) {
                    TPath symlink;
                    error = src.ReadlinkAt(name, symlink);
                    if (!error) {
                        (void)dir.UnlinkAt(name);
                        error = dir.SymlinkAt(name, symlink);
                    }
                } else
                    L("Skip {}", walk.Path);
            }

            error2 = old_cwd.Chdir();
            PORTO_ASSERT(!error2);

            if (error)
                return error;
        } else {
            auto name = path.BaseName();
            TProjectQuota::Toggle(dir, false);
            (void)dir.UnlinkAt(name);
            error = dir.HardlinkAt(name, src);
            TProjectQuota::Toggle(dir, true);
            if (error)
                return error;
        }
    }

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
        TFile dir;

        error = dir.OpenDir(StoragePath.DirNameNormal());
        if (error)
            return error;

        /* Loop image is read-only for user, but directory must be writable */
        error = CL->WriteAccess(dir);
        if (error)
            return error;

        error = StorageFd.OpenAt(dir, StoragePath.BaseNameNormal(),
                                 (IsReadOnly ? O_RDONLY : O_RDWR) |
                                 O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
    } else if (!RemoteStorage())
        error = StorageFd.OpenDir(StoragePath);
    if (error)
        return error;

    if (RemoteStorage()) {
        /* Nothing to check */
    } else if (UserStorage()) {
        if (IsReadOnly || StorageFd.IsRegular())
            error = CL->ReadAccess(StorageFd);
        else
            error = CL->WriteAccess(StorageFd);
        if (error) {
            error = TError(error, "Storage {}", Storage);
            if (config().volumes().insecure_user_paths())
                L("Ignore {}", error);
            else
                return error;
        }
    } else if (HaveStorage()) {
        TStorage storage;
        storage.Open(EStorageType::Storage, Place, Storage);
        error = storage.Load();
        if (error)
            return error;
        error = CL->CanControl(storage.Owner);
        if (error)
            return TError(error, "Storage {}", Storage);
        if (storage.Owner.IsUnknown()) {
            error = storage.SaveOwner(VolumeOwner);
            if (error)
                return error;
        }
        if (Private.empty())
            Private = storage.Private;
        else
            error = storage.SavePrivate(Private);
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

    if (BackendType != "overlay" && BackendType != "squash") {
        error = MergeLayers();
        if (error)
            return error;

        TFile base;
        if (RemoteStorage() || FileStorage())
            error = base.OpenDirStrict(InternalPath);
        else
            error = base.Dup(StorageFd);
        if (error)
            return error;

        error = MakeDirectories(base);
        if (error)
            return error;

        error = MakeSymlinks(base);
        if (error)
            return error;

        error = MakeShares(base, false);
        if (error)
            return error;
    }

    /* Initialize cred and perms but do not change if user hasn't asked */
    if (!IsReadOnly) {
        if (!KeepStorage || Spec->has_cred()) {
            error = InternalPath.Chown(VolumeCred);
            if (error)
                return error;
        }
        if (!KeepStorage || Spec->has_permissions()) {
            error = InternalPath.Chmod(VolumePermissions);
            if (error)
                return error;
        }
    }

    /* Get default device name from storage backend */
    if (!DeviceName.size() && StoragePath)
        TPath::GetDevName(StoragePath.GetDev(), DeviceName);

    /* Make sure than we saved this before publishing */
    error = Save();
    if (error)
        return error;

    StorageFd.Close();

    /* Keep storage only after successful build */
    if (!KeepStorage && HaveStorage())
        KeepStorage = true;

    BuildTime = time(nullptr);

    return OK;
}

TError TVolume::MountLink(std::shared_ptr<TVolumeLink> link) {
    TError error, error2;

    if (!link->Target)
        return OK;

    auto volumes_lock = LockVolumes();
    if (State != EVolumeState::Ready &&
            State != EVolumeState::Tuning &&
            State != EVolumeState::Building)
        return TError(EError::VolumeNotReady, "Volume {} not ready", Path);

    if (link->Volume.get() != this)
        return TError(EError::InvalidValue, "Wrong volume link");

    TPath host_target = link->Container->RootPath / link->Target;
    link->HostTarget = "";

    if (host_target != link->Volume->Path) {
        error = TVolume::CheckConflicts(host_target);
        if (error)
            return error;
    }

    link->HostTarget = host_target;

    auto prevLinkIt = VolumeLinks.find(link->HostTarget);
    if (prevLinkIt != VolumeLinks.end()) {
        L_WRN("Duplicate volume link: {}", link->HostTarget);

        for (auto ct = prevLinkIt->second->Container; ct; ct = ct->Parent)
            ct->VolumeMounts--;
    }

    VolumeLinks[link->HostTarget] = link;

    /* Block changes root path */
    for (auto ct = link->Container; ct; ct = ct->Parent)
        ct->VolumeMounts++;

    if (RootContainer->VolumeMounts != (int)VolumeLinks.size())
        L_WRN("Volume links index out of sync: {} != {}", RootContainer->VolumeMounts, VolumeLinks.size());

    if (!link->Busy)
        L_WRN("Link mount without protection: {}", host_target);

    volumes_lock.unlock();

    L_ACT("Mount volume {} link {} for CT{}:{} target {}", Path, host_target,
            link->Container->Id, link->Container->Name, link->Target);
    Statistics->VolumeLinksMounted++;

    TFile target_dir;
    TPath link_mount, real_target;
    std::unique_lock<std::mutex> internal_lock;
    unsigned long flags = 0;

    /* Prepare mountpoint */
    error = target_dir.OpenDirStrict(link->Container->RootPath);
    if (error)
        goto undo;

    for (auto &name: link->Target.Components()) {
        if (name == "/")
            continue;

        error = target_dir.OpenDirStrictAt(target_dir, name);
        if (!error)
            continue;

        if (error.Errno != ENOENT &&
                error.Errno != ELOOP &&
                error.Errno != ENOTDIR)
            break;

        /* Check permissions for change */
        if (CL->WriteAccess(target_dir))
            break;

        /* Remove symlink */
        if (error.Errno == ELOOP || error.Errno == ENOTDIR) {
            TPath symlink_target;
            if (target_dir.ReadlinkAt(name, symlink_target))
                break; /* Not at symlink */
            L_ACT("Remove symlink {} to {}", target_dir.RealPath() / name, symlink_target);
            error = target_dir.UnlinkAt(name);
            if (error)
                break;
        }

        L_ACT("Create directory {}", target_dir.RealPath() / name);
        error = target_dir.MkdirAt(name, 0775);
        if (error)
            break;
        error = target_dir.OpenDirStrictAt(target_dir, name);
        if (error)
            break;
        error = target_dir.Chown(CL->Cred);
        if (error)
            break;
    }
    if (error)
        goto undo;

    /* Sanity check */
    real_target = target_dir.RealPath();
    if (real_target != host_target) {
        error = TError(EError::InvalidPath, "Volume {} link {} real path is {}", Path, host_target, real_target);
        goto undo;
    }

    if (IsReadOnly || link->ReadOnly)
        flags |= MS_RDONLY;

    if (BackendType == "rbind" || BackendType == "dir")
        flags |= MS_REC;

    /* save state before changes */
    error = Save();
    if (error)
        goto undo;

    /* Several links can be created for one volume at a time */
    internal_lock = link->Volume->LockInternal();

    link_mount = GetInternal("volume_link");
    error = link_mount.Mkdir(700);

    /* make private - cannot move from shared mount */
    if (!error)
        error = link_mount.BindRemount(link_mount, MS_PRIVATE);

    /* Start new shared group and make read-only - that isn't propagated */
    if (!error)
        error = link_mount.BindRemount(InternalPath, flags | MS_SLAVE | MS_SHARED | MS_ALLOW_SUID);

    /* Move to target path and propagate into namespaces */
    if (!error)
        error = link_mount.MoveMount(target_dir.ProcPath());

    (void)link_mount.UmountAll();
    (void)link_mount.Rmdir();

    internal_lock.unlock();

    if (error)
        goto undo;

    return OK;

undo:
    volumes_lock.lock();

    if (link->HostTarget) {
        VolumeLinks.erase(link->HostTarget);
        for (auto ct = link->Container; ct; ct = ct->Parent)
            ct->VolumeMounts--;
        link->HostTarget = "";
    }

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
    TPath host_target = link->HostTarget;
    volumes_lock.unlock();

    L_ACT("Umount volume {} link {} for CT{}:{}", Path, host_target, link->Container->Id, link->Container->Name);

    error = host_target.Umount(UMOUNT_NOFOLLOW | (strict ? 0 : MNT_DETACH));
    if (error) {
        error = TError(error, "Cannot umount volume {} link {} for CT{}:{} target {}", Path,
                host_target, link->Container->Id, link->Container->Name, link->Target);
        if (error.Error == EError::NotFound)
            L("{}", error.Text);
        else if (strict)
            return error;
        else
            L_WRN("{}", error.Text);
    }

    volumes_lock.lock();

    for (auto it = VolumeLinks.lower_bound(host_target);
            it != VolumeLinks.end() && it->first.IsInside(host_target);) {
        auto &link = it->second;
        auto &vol = link->Volume;

        if (link->HostTarget != it->first)
            L_WRN("Volume link out of sync: {} != {}", link->HostTarget, it->first);

        L_ACT("Del volume {} link {} for CT{}:{}", vol->Path, link->Target, link->Container->Id, link->Container->Name);

        link->Container->VolumeLinks.remove(link);
        vol->Links.remove(link);

        if (link->HostTarget != host_target)
            L_ACT("Umount nested volume {} link {} for CT{}:{}",
                    link->Volume->Path, link->HostTarget,
                    link->Container->Id, link->Container->Name);

        /* Last or common link */
        if ((vol->Links.empty() || it->first == link->Volume->Path) &&
                (vol->State == EVolumeState::Ready || vol->State == EVolumeState::Tuning)) {
            link->Volume->SetState(EVolumeState::Unlinked);
            unlinked.emplace_back(link->Volume);
        }

        for (auto ct = link->Container; ct; ct = ct->Parent) {
            ct->VolumeMounts--;
            if (ct->VolumeMounts < 0)
                L_WRN("Volume mounts underflow {} at {} total {}", ct->VolumeMounts, ct->Name, VolumeLinks.size());
        }

        link->HostTarget = "";
        it = VolumeLinks.erase(it);
    }

    volumes_lock.unlock();

    /* Save changes only after umounting */
    (void)Save();

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

    if (CL->LockedContainer) {
        L_WRN("Locked container {} in TVolume::Destroy()", CL->LockedContainer->Name);
        Stacktrace();
        CL->ReleaseContainer();
    }

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
            if (!error)
                error = volume->UnlinkVolume(link->Container, link->Target, plan);
            CL->ReleaseContainer();

            volumes_lock.lock();
            if (error && link == volume->Links.back()) {
                L_WRN("Cannot unlink volume {}: {}", volume->Path, error);
                volume->Links.remove(link);
            }
        }

        volumes_lock.unlock();

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
            RootContainer->VolumeMounts--;

        if (volume->VolumeOwnerContainer) {
            volume->VolumeOwnerContainer->OwnedVolumes.remove(volume);
            volume->VolumeOwnerContainer = nullptr;
        }

        volume->SetState(EVolumeState::Destroyed);

        volumes_lock.unlock();

        for (auto &layer: volume->Layers) {
            TStorage storage;
            storage.Open(EStorageType::Layer, volume->Place, layer);
            if (storage.Weak()) {
                error = storage.Remove(true);
                if (error && error != EError::Busy)
                    L_WRN("Cannot remove weak layer {} : {}", layer, error);
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

    error = internal.UmountNested();
    if (error) {
        L_ERR("Cannot umount internal: {}", error);
        if (!ret)
            ret = error;
    }

    StorageFd.Close();

    if (KeepStorage && !UserStorage() && !RemoteStorage()) {
        TStorage storage;
        storage.Open(EStorageType::Storage, Place, Storage);
        if (storage.Weak()) {
            Storage = "";
            error = storage.Remove(true);
            Storage = storage.Name;
            if (error)
                L_WRN("Cannot remove storage {}: {}", storage.Path, error);
        } else {
            error = storage.Touch();
            if (error)
                L_WRN("Cannot touch storage: {}", error);
        }
        if (error && !ret)
            ret = error;
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
    if (State != EVolumeState::Ready &&
            State != EVolumeState::Tuning &&
            State != EVolumeState::Unlinked) {
        result.Reset();
        return TError(EError::VolumeNotReady, "Volume {} is not ready", Path);
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

    auto volumes_lock = LockVolumes();
    while (State == EVolumeState::Tuning)
        VolumesCv.wait(volumes_lock);
    if (State != EVolumeState::Ready)
        return TError(EError::VolumeNotReady, "Volume not ready: " + Path.ToString());
    SetState(EVolumeState::Tuning);
    volumes_lock.unlock();

    if (properties.count(V_SPACE_LIMIT) || properties.count(V_INODE_LIMIT)) {
        uint64_t spaceLimit = SpaceLimit, inodeLimit = InodeLimit;

        if (properties.count(V_SPACE_LIMIT)) {
            error = StringToSize(properties.at(V_SPACE_LIMIT), spaceLimit);
            if (error)
                goto out;
        }
        if (properties.count(V_INODE_LIMIT)) {
            error = StringToSize(properties.at(V_INODE_LIMIT), inodeLimit);
            if (error)
                goto out;
        }

        if (!spaceLimit || spaceLimit > SpaceLimit) {
            error = ClaimPlace(spaceLimit);
            if (error)
                goto out;
        }

        L_ACT("Resize volume: {} to bytes: {} inodes: {}",
                Path, spaceLimit, inodeLimit);

        error = Backend->Resize(spaceLimit, inodeLimit);
        if (error) {
            if (!spaceLimit || spaceLimit > SpaceLimit)
                ClaimPlace(SpaceLimit);
            goto out;
        }

        if (spaceLimit && spaceLimit < SpaceLimit)
            ClaimPlace(spaceLimit);

        volumes_lock.lock();
        SpaceLimit = spaceLimit;
        InodeLimit = inodeLimit;
        volumes_lock.unlock();
    }

    if (properties.count(V_SPACE_GUARANTEE) || properties.count(V_INODE_GUARANTEE)) {
        uint64_t space_guarantee = SpaceGuarantee, inode_guarantee = InodeGuarantee;

        if (properties.count(V_SPACE_GUARANTEE)) {
            error = StringToSize(properties.at(V_SPACE_GUARANTEE), space_guarantee);
            if (error)
                goto out;
        }
        if (properties.count(V_INODE_GUARANTEE)) {
            error = StringToSize(properties.at(V_INODE_GUARANTEE), inode_guarantee);
            if (error)
                goto out;
        }

        volumes_lock.lock();
        error = CheckGuarantee(space_guarantee, inode_guarantee);
        if (!error) {
            SpaceGuarantee = space_guarantee;
            InodeGuarantee = inode_guarantee;
        }
        volumes_lock.unlock();
    }

out:

    volumes_lock.lock();
    if (State == EVolumeState::Tuning)
        SetState(EVolumeState::Ready);
    volumes_lock.unlock();

    if (!error)
        Save();

    return error;
}

TError TVolume::Check() {
    return Backend->Check();
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

    PORTO_ASSERT(container->IsActionLocked());

    if (target) {
        if (!target.IsAbsolute())
            return TError(EError::InvalidPath, "Volume {} link path {} must be absolute", Path, target);
        if (!target.IsNormal())
            return TError(EError::InvalidPath, "Volume {} link path {} must be normalized", Path, target);
    }

    auto volumes_lock = LockVolumes();
    TPath host_target;
    TError error;

    for (auto &link: Links) {
        if (link->Container == container && link->Target == target)
            return TError(EError::VolumeAlreadyLinked, "Volume already linked");
    }

    if (State != EVolumeState::Ready &&
            State != EVolumeState::Tuning &&
            State != EVolumeState::Building)
        return TError(EError::VolumeNotReady, "Volume not ready: {}", Path);

    /* Mount link if volume is ready */
    if (target && (State == EVolumeState::Ready ||
                   State == EVolumeState::Tuning)) {
        host_target = container->RootPath / target;
        error = CheckConflicts(host_target);
        if (error)
            return error;
    }

    L_ACT("Add volume {} link {} for CT{}:{}", Path, target, container->Id, container->Name);

    auto link = std::make_shared<TVolumeLink>(shared_from_this(), container);
    link->Target = target;
    link->ReadOnly = read_only;
    link->Required = required;
    link->HostTarget = host_target;   /* protect path from conflicts */

    Links.emplace_back(link);
    container->VolumeLinks.emplace_back(link);

    bool was_required = std::find(container->RequiredVolumes.begin(),
            container->RequiredVolumes.end(), target.ToString()) != container->RequiredVolumes.end();

    if (required && !was_required)
        container->RequiredVolumes.emplace_back(target.ToString());

    if (!required && was_required)
        link->Required = true;

    if (link->Required && !HasDependentContainer)
        HasDependentContainer = true;

    link->Busy = true;

    volumes_lock.unlock();

    error = Save();
    if (error)
        goto undo;

    if (host_target) {
        error = MountLink(link);
        if (error)
            goto undo;
    }

    if (required && !was_required) {
        container->SetProp(EProperty::REQUIRED_VOLUMES);
        (void)container->Save();
    }

    link->Busy = false;

    return OK;

undo:
    volumes_lock.lock();
    link->Busy = false;
    Links.remove(link);
    container->VolumeLinks.remove(link);
    if (required && !was_required) {
        auto it = std::find(container->RequiredVolumes.begin(),
                            container->RequiredVolumes.end(), target.ToString());
        if (it != container->RequiredVolumes.end())
            container->RequiredVolumes.erase(it);
    }
    volumes_lock.unlock();
    return error;
}

TError TVolume::UnlinkVolume(std::shared_ptr<TContainer> container, const TPath &target,
                             std::list<std::shared_ptr<TVolume>> &unlinked, bool strict) {

    PORTO_ASSERT(container->IsActionLocked());

    TError error;
    auto volumes_lock = LockVolumes();
    std::shared_ptr<TVolumeLink> link;
    bool all = target.ToString() == "***";
    bool was_linked = false;

next:
    for (auto &l: Links) {
        if (l->Container == container && (all || l->Target == target)) {
            link = l;
            break;
        }
    }
    if (!link) {
        if (was_linked)
            return OK;
        return TError(EError::VolumeNotLinked, "Container {} is not linked with volume {}", container->Name, Path);
    }
    was_linked = true;

    /* If strict then fail if volume is used */

    if (strict && Links.size() > 1)
        return TError(EError::Busy, "More than one container linked with volume {}", Path);

    if (strict && !Nested.empty())
        return TError(EError::Busy, "Volume {} has sub-volumes", Path);

    if (link->Busy)
        return TError(EError::Busy, "Volume {} link {} is busy", Path, target);

    if (link->HostTarget) {
        link->Busy = true;
        volumes_lock.unlock();
        error = UmountLink(link, unlinked, strict);
        link->Busy = false;
        if (error) {
            if (strict)
                return error;
            L_WRN("Cannot umount volume link: {}", error);
        }
        volumes_lock.lock();
    }

    L_ACT("Del volume {} link {} for CT{}:{}", Path, link->Target, container->Id, container->Name);

    Links.remove(link);
    if (Links.empty() && (State == EVolumeState::Ready ||
                          State == EVolumeState::Tuning)) {
        SetState(EVolumeState::Unlinked);
        unlinked.emplace_back(shared_from_this());
    }
    container->VolumeLinks.remove(link);
    /* Required path at container is sticky */
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

        error = volume->Save(true);

        if (error)
            L_WRN("Cannot save volume {}: {}", volume->Path, error);
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
        if (link->Volume->State != EVolumeState::Ready &&
                link->Volume->State != EVolumeState::Tuning)
            return TError(EError::VolumeNotReady, "Required volume {} not ready", path);
        link->Volume->HasDependentContainer = true;
    }
    return OK;
}

void TVolume::DumpDescription(TVolumeLink *link, const TPath &path, rpc::TVolumeDescription *dump) {
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
    if (VolumeCred.GetUid() != NoUser)
        ret[V_USER] = VolumeCred.User();
    if (VolumeCred.GetGid() != NoGroup)
        ret[V_GROUP] = VolumeCred.Group();
    ret[V_PERMISSIONS] = fmt::format("{:#o}", VolumePermissions);
    ret[V_CREATOR] = Creator;
    ret[V_READY] = BoolToString(State == EVolumeState::Ready ||
                                State == EVolumeState::Tuning);
    if (BuildTime)
        ret[V_BUILD_TIME] = FormatTime(BuildTime);
    ret[V_CHANGE_TIME] = FormatTime(ChangeTime);
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

    ret[V_PLACE] = Place.ToString();

    if (DeviceName.size())
        ret[V_DEVICE_NAME] = DeviceName;

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

    ret[V_SPACE_USED] = std::to_string(Stat.SpaceUsage);
    ret[V_INODE_USED] = std::to_string(Stat.InodeUsage);
    ret[V_SPACE_AVAILABLE] = std::to_string(Stat.SpaceAvail);
    ret[V_INODE_AVAILABLE] = std::to_string(Stat.InodeAvail);

    dump->set_path(path.ToString());
    dump->set_change_time(ChangeTime);

    for (auto &prop: ret) {
        auto p = dump->add_properties();
        p->set_name(prop.first);
        p->set_value(prop.second);
    }
}

void TVolume::UpdateStatFS() {
    StatFS(Stat);
}

TError TVolume::Save(bool locked) {
    TKeyValue node(VolumesKV / Id);
    TError error;
    std::string tmp;

    std::unique_lock<std::mutex> volumes_lock;

    if (!locked)
        volumes_lock = LockVolumes();

    if (State == EVolumeState::ToDestroy ||
            State == EVolumeState::Destroying ||
            State == EVolumeState::Destroyed)
        return OK;

    ChangeTime = time(nullptr);

    /*
     * Storing all state values on save,
     * the previous scheme stored knobs selectively.
     */

    node.Set(V_RAW_ID, Id);
    node.Set(V_PATH, Path.ToString());
    node.Set(V_AUTO_PATH, BoolToString(IsAutoPath));
    node.Set(V_STORAGE, Storage);
    node.Set(V_BACKEND, BackendType);

    node.Set(V_CREATOR, Creator);
    node.Set(V_BUILD_TIME, std::to_string(BuildTime));

    if (VolumeOwnerContainer)
        node.Set(V_OWNER_CONTAINER, VolumeOwnerContainer->Name);

    node.Set(V_OWNER_USER, VolumeOwner.User());
    node.Set(V_OWNER_GROUP, VolumeOwner.Group());

    if (VolumeCred.GetUid() != NoUser)
        node.Set(V_USER, VolumeCred.User());
    if (VolumeCred.GetGid() != NoGroup)
        node.Set(V_GROUP, VolumeCred.Group());

    node.Set(V_PERMISSIONS, fmt::format("{:#o}", VolumePermissions));
    node.Set(V_READY, BoolToString(State == EVolumeState::Ready ||
                                   State == EVolumeState::Tuning));
    node.Set(V_PRIVATE, Private);
    node.Set(V_LOOP_DEV, std::to_string(DeviceIndex));
    node.Set(V_READ_ONLY, BoolToString(IsReadOnly));
    node.Set(V_LAYERS, MergeEscapeStrings(Layers, ';'));
    node.Set(V_SPACE_LIMIT, std::to_string(SpaceLimit));
    node.Set(V_SPACE_GUARANTEE, std::to_string(SpaceGuarantee));
    node.Set(V_INODE_LIMIT, std::to_string(InodeLimit));
    node.Set(V_INODE_GUARANTEE, std::to_string(InodeGuarantee));

    if (DeviceName.size())
        node.Set(V_DEVICE_NAME, DeviceName);

    TMultiTuple links;
    for (auto &link: Links) {
        if (link->Target)
            links.push_back({link->Container->Name,
                             link->Target.ToString(),
                             link->ReadOnly ? "ro" : "rw",
                             link->Required ? "!" : ".",
                             link->HostTarget.ToString()});
        else
            links.push_back({link->Container->Name});
    }
    node.Set(V_RAW_CONTAINERS, MergeEscapeStrings(links, ' ', ';'));

    node.Set(V_PLACE, Place.ToString());

    error = node.Save();
    if (error)
        L_WRN("Cannot save volume {} {}", Path, error);

    return error;
}

TError TVolume::Restore(const TKeyValue &node) {
    rpc::TVolumeSpec spec;
    TError error;

    error = ParseConfig(node.Data, spec);
    if (error)
        return error;

    if (!spec.has_id())
        return TError(EError::InvalidValue, "No volume id stored");

    error = Load(spec, true);
    if (error)
        return error;

    if (!spec.has_place())
        Place = PORTO_PLACE;

    InternalPath = Place / PORTO_VOLUMES / Id / "volume";

    if (!spec.has_owner())
        VolumeOwner = VolumeCred;

    if (spec.has_owner_container()) {
        error = CL->WriteContainer(spec.owner_container(), VolumeOwnerContainer, true);
        CL->ReleaseContainer();
        if (error)
            L_WRN("Cannot find volume owner: {}", error);
    }

    if (!VolumeOwnerContainer)
        VolumeOwnerContainer = RootContainer;

    VolumeOwnerContainer->OwnedVolumes.push_back(shared_from_this());

    if (!HaveStorage())
        StoragePath = GetInternal(BackendType);
    else if (!UserStorage() && !RemoteStorage()) {
        TStorage storage;
        storage.Open(EStorageType::Storage, Place, Storage);
        StoragePath = storage.Path;
    }

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Restore();
    if (error)
        return error;

    error = ClaimPlace(SpaceLimit);
    if (error)
        return error;

    if (!DeviceName.size() && StoragePath)
        TPath::GetDevName(StoragePath.GetDev(), DeviceName);

    if (Volumes.find(Path) != Volumes.end())
        L_WRN("Duplicate volume: {}", Path);

    if (VolumeLinks.find(Path) != VolumeLinks.end())
        L_WRN("Duplicate volume link: {}", Path);

    Volumes[Path] = shared_from_this();

    /* Restore common link */
    auto common_link = std::make_shared<TVolumeLink>(shared_from_this(), RootContainer);
    common_link->Target = Path;
    common_link->HostTarget = Path;
    common_link->ReadOnly = IsReadOnly;
    VolumeLinks[Path] = common_link;
    RootContainer->VolumeMounts++;

    /* Restore other links */
    auto containers_lock = LockContainers();
    for (auto &l: spec.links()) {
        std::shared_ptr<TContainer> ct;
        bool placeholder = false;

        error = TContainer::Find(l.container(), ct);
        if (error) {
            error = OK;
            L("Volume is linked to missing container {}", l.container());
            if (l.has_host_target()) {
                placeholder = true;
                ct = RootContainer;
            } else
                continue;
        }

        auto link = std::make_shared<TVolumeLink>(shared_from_this(), ct);
        if (l.has_target())
            link->Target = placeholder ? "placeholder" : l.target();
        link->HostTarget = l.host_target();
        link->ReadOnly = l.read_only();
        link->Required = l.required();

        if (link->HostTarget && VolumeLinks.find(link->HostTarget) != VolumeLinks.end()) {
            L_WRN("Drop duplicate volume target: {}", link->HostTarget);
            link->Target = placeholder ? "placeholder" : "";
            link->HostTarget = "";
        }

        bool duplicate = false;
        for (auto &l: ct->VolumeLinks) {
            if (link->Volume == l->Volume && link->Target == l->Target)
                duplicate = true;
        }
        if (duplicate) {
            L_WRN("Duplicate volume {} link {} for CT{}:{} target {}", Path, link->HostTarget,
                    link->Container->Id, link->Container->Name, link->Target);
            continue;
        }

        if (link->HostTarget) {
            L("Restore volume {} link {} for CT{}:{} target {}", Path, link->HostTarget,
                    link->Container->Id, link->Container->Name, link->Target);

            TMount mount;
            error = link->HostTarget.FindMount(mount, true);
            if (error) {
                L("Link is lost: {}", error);
                continue;
            }

            VolumeLinks[link->HostTarget] = link;
            for (auto c = ct; c; c = c->Parent)
                c->VolumeMounts++;
        }

        Links.emplace_back(link);
        ct->VolumeLinks.emplace_back(link);
    }

    UpdateStatFS();

    return OK;
}

std::vector<TVolumeProperty> VolumeProperties = {
    { V_BACKEND,     "dir|plain|bind|rbind|tmpfs|hugetmpfs|quota|native|overlay|squash|lvm|loop|rbd (default - autodetect)", false },
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
    { V_CONTAINERS,  "container [target] [ro] [!];... - initial links (default - self)", false },
    { V_LAYERS,      "top-layer;...;bottom-layer - overlayfs layers", false },
    { V_PLACE,       "place for layers and default storage (optional)", false },
    { V_DEVICE_NAME, "name of backend disk device (ro)", true },
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

std::vector<std::string> AuxPlacesPaths;

TError TVolume::Create(const rpc::TVolumeSpec &spec,
                       std::shared_ptr<TVolume> &volume) {
    TError error;

    if (!CL)
        return TError("no client");

    L_VERBOSE("Volume spec: {}", spec.ShortDebugString());

    if (spec.private_value().size() > PRIVATE_VALUE_MAX)
        return TError(EError::InvalidValue, "Private value too log, max {} bytes", PRIVATE_VALUE_MAX);

    TPath place = spec.place();
    error = CL->ClientContainer->ResolvePlace(place);
    if (error)
        return error;

    error = TStorage::CheckPlace(place);
    if (error)
        return error;

    std::shared_ptr<TContainer> owner;
    TPath target_root;

    if (spec.has_container()) {
        std::shared_ptr<TContainer> target;
        error = CL->WriteContainer(spec.container(), target, true);
        if (error)
            return error;
        target_root = target->RootPath;
        CL->ReleaseContainer();
    } else
        target_root = CL->ClientContainer->RootPath;

    if (spec.has_owner_container()) {
        error = CL->WriteContainer(spec.owner_container(), owner, true);
    } else {
        owner = CL->ClientContainer;
        error = CL->LockContainer(owner);
    }
    if (error)
        return error;

    volume = std::make_shared<TVolume>();
    volume->Id = std::to_string(NextId++);
    volume->Spec = &spec;

    /* Default user:group */
    volume->VolumeOwner = CL->Cred;
    volume->VolumeCred = CL->TaskCred;
    volume->Creator = CL->ClientContainer->Name + " " + CL->Cred.User() + " " + CL->Cred.Group();

    error = volume->Load(spec);
    if (error)
        return error;

    auto max_vol = config().volumes().max_total();
    if (CL->IsSuperUser())
        max_vol += NR_SUPERUSER_VOLUMES;

    auto volumes_lock = LockVolumes();

    if (Volumes.size() >= max_vol)
        return TError(EError::ResourceNotAvailable, "number of volumes reached limit: " + std::to_string(max_vol));

    if (spec.has_path()) {
        TPath path = spec.path();

        if (!path.IsAbsolute())
            return TError(EError::InvalidPath, "Volume path must be absolute");

        if (!path.IsNormal())
            return TError(EError::InvalidPath, "Volume path must be normalized");

        path = target_root / path;

        if (Volumes.count(path))
            return TError(EError::VolumeAlreadyExists, "Volume already exists");

        TFile path_dir;
        error = path_dir.OpenDir(path);
        if (error)
            return TError(EError::InvalidPath, "Cannot open volume path: {}", error);

        if (!path_dir.IsDirectory())
            return TError(EError::InvalidPath, "Volume path {} must be a directory", path);

        TPath real_path = path_dir.RealPath();
        if (real_path != path)
            return TError(EError::InvalidPath, "Volume {} real path is {}", path, real_path);

        error = CheckConflicts(path);
        if (error)
            return error;
    }

    error = volume->Configure(target_root);
    if (error)
        return error;

    /* Add common link */
    auto common_link = std::make_shared<TVolumeLink>(volume, RootContainer);
    common_link->Target = volume->Path;
    common_link->HostTarget = volume->Path;
    common_link->ReadOnly = volume->IsReadOnly;
    common_link->Busy = true;

    if (volume->Path == volume->InternalPath) {
        VolumeLinks[volume->Path] = common_link;
        RootContainer->VolumeMounts++;
    }

    /* also check if volume depends on itself */
    error = volume->CheckDependencies();
    if (error) {
        VolumeLinks.erase(volume->Path);
        if (volume->Path == volume->InternalPath)
            RootContainer->VolumeMounts--;

        return error;
    }

    Volumes[volume->Path] = volume;

    volume->VolumeOwnerContainer = owner;
    owner->OwnedVolumes.push_back(volume);

    volume->SetState(EVolumeState::Building);

    volumes_lock.unlock();

    error = volume->ClaimPlace(volume->SpaceLimit);

    /* release owner */
    CL->ReleaseContainer();

    if (error)
        goto undo;

    error = volume->Build();
    volume->Spec = nullptr;
    if (error)
        goto undo;

    if (spec.links().size()) {
        for (auto &link: spec.links()) {
            std::shared_ptr<TContainer> ct;
            error = CL->WriteContainer(link.container(), ct, true);
            if (error)
                goto undo;
            error = volume->LinkVolume(ct, link.target(), link.read_only(), link.required());

            if (!error && link.container_root() && ct->Parent) {
                ct->LockStateWrite();
                error = ct->SetProperty(P_ROOT, ct->Parent->RootPath.InnerPath(volume->Path).ToString());
                ct->UnlockState();
            }

            if (!error && link.container_cwd()) {
                ct->LockStateWrite();
                error = ct->SetProperty(P_CWD, ct->RootPath.InnerPath(volume->Path).ToString());
                ct->UnlockState();
            }

            CL->ReleaseContainer();
            if (error)
                goto undo;
        }
    } else {
        error = CL->LockContainer(CL->ClientContainer);
        if (!error)
            error = volume->LinkVolume(CL->ClientContainer);
        CL->ReleaseContainer();
        if (error)
            goto undo;
    }

    error = volume->Save();
    if (error)
        goto undo;

    /* Mount common link in requested path */
    if (volume->Path != volume->InternalPath) {
        error = volume->MountLink(common_link);
        if (error)
            goto undo;
    }

    /* Mount other links */
next_link:
    volumes_lock.lock();
    for (auto link: volume->Links) {
        if (link->Target && !link->HostTarget) {
            link->Busy = true;
            volumes_lock.unlock();
            error = volume->MountLink(link);
            link->Busy = false;
            if (error)
                goto undo;
            goto next_link;
        }
    }

    /* Complete costriction */
    volume->SetState(EVolumeState::Ready);
    common_link->Busy = false;
    volumes_lock.unlock();

    /* Final commit */
    error = volume->Save();
    if (error)
        goto undo;

    if (volume->SpaceLimit) {
        volume->CacheQuotaFile();
    }

    volume->UpdateStatFS();

    return OK;

undo:
    common_link->Busy = false;
    volume->Destroy();
    return error;
}

void TVolume::RestoreAll(void) {
    std::list<TKeyValue> nodes;
    TError error;

    TStorage def_place;
    std::vector<TStorage> aux_places;

    AuxPlacesPaths = SplitString(config().volumes().aux_default_places(), ';');

    def_place.Open(EStorageType::Place, PORTO_PLACE);
    error = TStorage::CheckPlace(def_place.Path);
    if (error)
        L_ERR("Cannot prepare place {}: {}", def_place.Path, error);

    for (const auto &path : AuxPlacesPaths) {
        TStorage aux_place;

        aux_place.Open(EStorageType::Place, path);
        error = TStorage::CheckPlace(aux_place.Path);
        if (error)
            L_ERR("Cannot prepare place {}: {}", aux_place.Path, error);

        aux_places.push_back(aux_place);
    }

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
            L_WRN("Volume {} restore: {}", node.Path, error);
            broken_volumes.push_back(volume);
            continue;
        }

        uint64_t id;
        if (!StringToUint64(volume->Id, id)) {
            if (id >= NextId)
                NextId = id + 1;
        }

        error = volume->Save();
        if (error) {
            broken_volumes.push_back(volume);
            continue;
        }

        error = volume->CheckDependencies();
        if (error) {
            L("Volume {} has broken dependencies: {}", volume->Path, error);
            broken_volumes.push_back(volume);
            continue;
        }

        if (volume->State != EVolumeState::Ready) {
            L("Volume {} is not ready ({}) and will be removed", volume->Path, StateName(volume->State));
            broken_volumes.push_back(volume);
            continue;
        }

        if (volume->BackendType != "dir" && volume->BackendType != "quota") {
            TMount mount;
            error = volume->Path.FindMount(mount, true);
            if (error) {
                L("Volume {} is not mounted: {}", volume->Path, error);
                broken_volumes.push_back(volume);
                continue;
            }
        }

        if (!volume->Links.size()) {
            L("Volume {} has no linked containers", volume->Path);
            volume->SetState(EVolumeState::Unlinked);
            broken_volumes.push_back(volume);
            continue;
        }

next_link:
        for (auto &link: volume->Links) {
            /* remove placeholder links */
            if (link->Container == RootContainer &&
                    link->Target.ToString() == "placeholder") {
                L("Remove placeholder link {} for missing container", link->HostTarget);
                CL->LockContainer(RootContainer);
                error = volume->UnlinkVolume(RootContainer, link->Target, broken_volumes);
                CL->ReleaseContainer();
                if (error)
                    volume->Links.remove(link);
                goto next_link;
            }
        }

        if (RootContainer->VolumeMounts != (int)VolumeLinks.size())
            L_WRN("Volume links index out of sync: {} != {}", RootContainer->VolumeMounts, VolumeLinks.size());

        if (volume->SpaceLimit) {
            volume->CacheQuotaFile();
        }

        L("Volume {} restored", volume->Path);
    }

    L_SYS("Remove broken volumes...");

    for (auto &volume : broken_volumes) {
        Statistics->VolumeLost++;
        error = volume->Destroy();
        if (error)
            L_WRN("Volume {} destroy: {}", volume->Path, error);
    }

    L_SYS("Remove stale volumes...");

    std::vector<TPath> volumes;

    auto all_def_places = aux_places;
    all_def_places.push_back(def_place);

    for (const auto &place : all_def_places)
        volumes.push_back(place.Path / PORTO_VOLUMES);

    for (const auto &volume : volumes) {
        std::vector<std::string> subdirs;

        error = volume.ReadDirectory(subdirs);
        if (error)
            L_ERR("Cannot list {}", volume);

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

            TPath dir = volume / dir_name;

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
    }

    L_SYS("Remove stale layers...");

    std::list<TStorage> storages;

    for (auto &place : all_def_places) {
        storages.clear();
        error = place.List(EStorageType::Layer, storages);
        if (error) {
            L_WRN("Layers listing failed : {}", error);
        } else {
            for (auto &layer : storages) {
                if (layer.Weak()) {
                    error = layer.Remove(true);
                    if (error && error != EError::Busy)
                        L_WRN("Cannot remove layer {} : {}", layer.Name, error);
                }
            }
        }
    }

    L_SYS("Remove stale storages...");

    for (auto &place : all_def_places) {
        storages.clear();
        error = place.List(EStorageType::Storage, storages);
        if (error) {
            L_WRN("Storage listing failed : {}", error);
        } else {
            for (auto &storage : storages) {
                if (storage.Weak()) {
                    error = storage.Remove(true);
                    if (error && error != EError::Busy)
                        L_WRN("Cannot remove storage {} : {}", storage.Name, error);
                }
            }
        }
    }
}

TError TVolume::VerifyConfig(const TStringMap &cfg) {

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
            return TError(EError::InvalidProperty, "Unknown property: " + it.first);
        if (prop->ReadOnly)
            return TError(EError::InvalidProperty, "Read-only property: " + it.first);
    }

    return OK;
}

TError TVolume::ParseConfig(const TStringMap &cfg, rpc::TVolumeSpec &spec) {

    for (auto &it : cfg) {
        auto &key = it.first;
        auto &val = it.second;
        TError error;

        if (key == V_RAW_ID) {
            spec.set_id(val);
        } else if (key == V_PATH) {
            spec.set_path(val);
        } else if (key == V_AUTO_PATH) {
            bool v;
            error = StringToBool(val, v);
            spec.set_auto_path(v);
        } else if (key == V_TARGET_CONTAINER) {
            spec.set_container(val);
        } else if (key == V_PLACE) {
            spec.set_place(val);
        } else if (key == V_STORAGE) {
            spec.set_storage(val);
        } else if (key == V_LOOP_DEV) {
            int v;
            error = StringToInt(val, v);
            spec.set_device_index(v);;
        } else if (key == V_DEVICE_NAME) {
            spec.set_device_name(val);
        } else if (key == V_BACKEND) {
            spec.set_backend(val);
        } else if (key == V_OWNER_CONTAINER) {
            spec.set_owner_container(val);
        } else if (key == V_OWNER_USER) {
            spec.mutable_owner()->set_user(val);
        } else if (key == V_OWNER_GROUP) {
            spec.mutable_owner()->set_group(val);
        } else if (key == V_USER) {
            spec.mutable_cred()->set_user(val);
        } else if (key == V_GROUP) {
            spec.mutable_cred()->set_group(val);
        } else if (key == V_PERMISSIONS) {
            unsigned v;
            error = StringToOct(val, v);
            spec.set_permissions(v);
        } else if (key == V_PRIVATE) {
            spec.set_private_value(val);
        } else if (key == V_CONTAINERS) {
            for (auto &l: SplitEscapedString(val, ' ', ';')) {
                auto link = spec.add_links();
                link->set_container(l[0]);
                if (l.size() > 1)
                    link->set_target(l[1]);
                if (l.size() > 2 && l[2] == "ro")
                    link->set_read_only(true);
                if ((l.size() > 2 && l[2] == "!") ||
                        (l.size() > 3 && l[3] == "!"))
                    link->set_required(true);
            }
        } else if (key == V_RAW_CONTAINERS) {
            for (auto &l: SplitEscapedString(val, ' ', ';')) {
                auto link = spec.add_links();
                link->set_container(l[0]);
                if (l.size() > 1)
                    link->set_target(l[1]);
                if (l.size() > 2 && l[2] == "ro")
                    link->set_read_only(true);
                if (l.size() > 3 && l[3] == "!")
                    link->set_required(true);
                if (l.size() > 4)
                    link->set_host_target(l[4]);
            }
        } else if (key == V_READ_ONLY) {
            bool v;
            error = StringToBool(val, v);
            spec.set_read_only(v);
        } else if (key == V_LAYERS) {
            for (auto &l: SplitEscapedString(val, ';'))
                spec.add_layers(l);
        } else if (key == V_SPACE_LIMIT) {
            uint64_t v;
            error = StringToSize(val, v);
            spec.mutable_space()->set_limit(v);
        } else if (key == V_SPACE_GUARANTEE) {
            uint64_t v;
            error = StringToSize(val, v);
            spec.mutable_space()->set_guarantee(v);
        } else if (key == V_INODE_LIMIT) {
            uint64_t v;
            error = StringToSize(val, v);
            spec.mutable_inodes()->set_limit(v);
        } else if (key == V_INODE_GUARANTEE) {
            uint64_t v;
            error = StringToSize(val, v);
            spec.mutable_inodes()->set_guarantee(v);
        } else if (key == V_CREATOR) {
            spec.set_creator(val);
        } else if (key == V_BUILD_TIME) {
            uint64_t v;
            if (StringToUint64(val, v))
                v = time(nullptr);
            spec.set_build_time(v);
        } else if (key == V_READY) {
            spec.set_state(val == "true" ? "ready" : "unknown");
        } else if (key == V_STATE) {
            spec.set_state(val);
        } else
            return TError(EError::InvalidProperty, "Unknown volume property {} = {}", key, val);
        if (error)
            return error;
    }

    return OK;
}

TError TVolume::Load(const rpc::TVolumeSpec &spec, bool full) {
    TError error, ret;

    if (spec.has_id() && full)
        Id = spec.id();

    if (spec.has_path())
        Path = spec.path();

    if (spec.has_auto_path() && full)
        IsAutoPath = spec.auto_path();

    if (spec.has_storage()) {
        Storage = spec.storage();
        StoragePath = Storage;
        KeepStorage = HaveStorage();
    }

    if (spec.has_backend())
        BackendType = spec.backend();

    if (spec.has_device_index() && full)
        DeviceIndex = spec.device_index();

    if (spec.has_device_name() && full)
        DeviceName = spec.device_name();

    if (spec.has_place())
        Place = spec.place();

    if (spec.has_owner())
        error = VolumeOwner.Load(spec.owner());
    if (error)
        return error;

    if (spec.has_cred())
        error = VolumeCred.Load(spec.cred(), false);
    if (error)
        return error;

    if (spec.has_permissions())
        VolumePermissions = spec.permissions();

    if (spec.has_state() && full)
        SetState(spec.state() == "ready" ? EVolumeState::Ready : EVolumeState::ToDestroy);

    if (spec.has_private_value())
        Private = spec.private_value();

    if (spec.has_read_only())
        IsReadOnly = spec.read_only();

    for (auto &l: spec.layers())
        Layers.push_back(l);

    if (spec.has_space()) {
        SpaceLimit = spec.space().limit();
        SpaceGuarantee = spec.space().guarantee();
    }

    if (spec.has_inodes()) {
        InodeLimit = spec.inodes().limit();
        InodeGuarantee = spec.inodes().guarantee();
    }

    if (spec.has_build_time() && full)
        BuildTime = spec.build_time();

    if (spec.has_creator() && full)
        Creator = spec.creator();

    return ret;
}

void TVolume::Dump(rpc::TVolumeSpec &spec, bool full) {
    auto volumes_lock = LockVolumes();

    spec.set_path(CL->ComposePath(Path).ToString());
    spec.set_container(CL->RelativeName(CL->ClientContainer->Name));

    if (IsAutoPath)
        spec.set_auto_path(true);

    spec.set_id(Id);
    spec.set_state(StateName(State));
    spec.set_backend(BackendType);

    if (Private.size())
        spec.set_private_value(Private);

    spec.set_creator(Creator);
    spec.set_build_time(BuildTime);
    spec.set_change_time(ChangeTime);

    spec.set_place(Place.ToString());

    if (DeviceName.size())
        spec.set_device_name(DeviceName);

    if (HaveStorage()) {
        if (UserStorage() && !RemoteStorage())
            spec.set_storage(CL->ComposePath(StoragePath).ToString());
        else
            spec.set_storage(Storage);
    }

    if (DeviceIndex >= 0)
        spec.set_device_index(DeviceIndex);

    if (VolumeOwnerContainer)
        spec.set_owner_container(CL->RelativeName(VolumeOwnerContainer->Name));

    if (!VolumeOwner.IsUnknown())
        VolumeOwner.Dump(*spec.mutable_owner());

    if (!VolumeCred.IsUnknown())
        VolumeCred.Dump(*spec.mutable_cred());

    spec.set_permissions(VolumePermissions);
    spec.set_read_only(IsReadOnly);

    if (SpaceLimit)
        spec.mutable_space()->set_limit(SpaceLimit);
    if (SpaceGuarantee)
        spec.mutable_space()->set_guarantee(SpaceGuarantee);
    if (InodeLimit)
        spec.mutable_inodes()->set_limit(InodeLimit);
    if (InodeGuarantee)
        spec.mutable_inodes()->set_guarantee(InodeGuarantee);

    for (auto &layer: Layers) {
        TPath path(layer);
        if (path.IsAbsolute())
            path = CL->ComposePath(path);
        spec.add_layers(path.ToString());
    }

    if (Backend)
        spec.set_place_key(Backend->ClaimPlace());

    for (auto &link: Links) {
        auto l = spec.add_links();
        l->set_container(CL->RelativeName(link->Container->Name));
        if (link->Target)
            l->set_target(link->Target.ToString());
        if (link->ReadOnly)
            l->set_read_only(true);
        if (link->Required)
            l->set_required(true);
        if (link->HostTarget)
            l->set_host_target(link->HostTarget.ToString());
    }

    volumes_lock.unlock();

    if (!full) {
        spec.mutable_space()->set_usage(Stat.SpaceUsage);
        spec.mutable_space()->set_available(Stat.SpaceAvail);
        spec.mutable_inodes()->set_usage(Stat.InodeUsage);
        spec.mutable_inodes()->set_available(Stat.InodeAvail);
    }
}
