#include "util/proc.hpp"
#include "quota.hpp"
#include "log.hpp"
#include "config.hpp"
#include <mutex>

extern "C" {
#include <linux/quota.h>
#include <linux/dqblk_xfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ftw.h>
}

#ifndef PRJQUOTA
#define PRJQUOTA 2
#endif

#ifndef DQF_SYS_FILE
#define DQF_SYS_FILE    0x10000
#endif

struct fsxattr {
    __u32 fsx_xflags;
    __u32 fsx_extsize;
    __u32 fsx_nextents;
    __u32 fsx_projid;
    unsigned char fsx_pad[12];
};

#ifndef FS_IOC_FSGETXATTR
#define FS_IOC_FSGETXATTR   _IOR('X', 31, struct fsxattr)
#endif

#ifndef FS_IOC_FSSETXATTR
#define FS_IOC_FSSETXATTR   _IOW('X', 32, struct fsxattr)
#endif

#ifndef FS_XFLAG_PROJINHERIT
#define FS_XFLAG_PROJINHERIT    0x00000200
#endif

/* First generic header */
struct v2_disk_dqheader {
    __le32 dqh_magic;    /* Magic number identifying file */
    __le32 dqh_version;  /* File version */
};

/* Header with type and version specific information */
struct v2_disk_dqinfo {
    __le32 dqi_bgrace;   /* Time before block soft limit becomes hard limit */
    __le32 dqi_igrace;   /* Time before inode soft limit becomes hard limit */
    __le32 dqi_flags;    /* Flags for quotafile (DQF_*) */
    __le32 dqi_blocks;   /* Number of blocks in file */
    __le32 dqi_free_blk; /* Number of first free block in the list */
    __le32 dqi_free_entry;    /* Number of block with at least one free entry */
};

static std::mutex QuotaMutex;

static inline std::unique_lock<std::mutex> LockQuota() {
    return std::unique_lock<std::mutex>(QuotaMutex);
}

TError TProjectQuota::InitProjectQuotaFile(const TPath &path) {
    struct {
        struct v2_disk_dqheader header;
        struct v2_disk_dqinfo info;
        char zero[1024 * 2 - 8 * 4];
    } quota_init = {
        .header = {
            .dqh_magic = PROJECT_QUOTA_MAGIC,
            .dqh_version = 1,
        },
        .info = {
            .dqi_bgrace = 7 * 24 * 60 * 60,
            .dqi_igrace = 7 * 24 * 60 * 60,
            .dqi_flags = 0,
            .dqi_blocks = 2, /* header and root */
            .dqi_free_blk = 0,
            .dqi_free_entry = 0,
        },
        .zero = {0, },
    };
    int fd, ret, saved_errno;

    fd = open(path.c_str(), O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return TError::System("Cannot create quota file");
    ret = write(fd, &quota_init, sizeof(quota_init));
    saved_errno = errno;
    fsync(fd);
    close(fd);
    if (ret == sizeof(quota_init))
        return OK;
    if (ret >= 0)
        saved_errno = EIO;
    return TError(EError::Unknown, saved_errno, "Cannot write quota file");
}

TError TProjectQuota::Enable() {
    struct fs_quota_statv statv;
    struct dqinfo info;
    TError error;
    int ret;

    error = FindDevice();
    if (error)
        return error;

    statv.qs_version = FS_QSTATV_VERSION1;
    if (!quotactl(QCMD(Q_XGETQSTATV, PRJQUOTA), Device.c_str(),
                  0, (caddr_t)&statv)) {
        if ((statv.qs_flags & FS_QUOTA_PDQ_ACCT) &&
                (statv.qs_flags & FS_QUOTA_PDQ_ENFD))
            return OK;
    }

    auto lock = LockQuota();

    if (!quotactl(QCMD(Q_GETINFO, PRJQUOTA), Device.c_str(),
                  0, (caddr_t)&info)) {
        if (!(info.dqi_flags & DQF_SYS_FILE) ||
            !quotactl(QCMD(Q_QUOTAON, PRJQUOTA), Device.c_str(),
                      0, NULL) || errno == EEXIST)
            return OK;
        return TError(EError::NotSupported, errno, "Cannot enable project quota");
    }

    ret = mount(NULL, RootPath.c_str(), NULL, MS_REMOUNT, "quota");
    if (ret)
        return TError(EError::NotSupported, errno, "Cannot enable project quota");

    TPath quota = RootPath / PROJECT_QUOTA_FILE;
    if (!quota.Exists()) {
        error = InitProjectQuotaFile(quota);
        if (error)
            return error;
    }

    ret = quotactl(QCMD(Q_QUOTAON, PRJQUOTA), Device.c_str(),
                   QFMT_VFS_V1, (caddr_t)quota.c_str());
    if (ret)
        error = TError(EError::NotSupported, errno, "Cannot enable project quota");

    return error;
}

TError TProjectQuota::GetProjectId(const TPath &path, uint32_t &id) {
    struct fsxattr attr;
    int fd, ret, saved_errno;

    fd = open(path.c_str(), O_CLOEXEC | O_RDONLY | O_NOCTTY |
                            O_NOFOLLOW | O_NOATIME | O_NONBLOCK);
    if (fd < 0 && errno == EPERM)
        fd = open(path.c_str(), O_CLOEXEC | O_RDONLY | O_NOCTTY |
                                O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        fd = open(path.DirName().c_str(), O_RDONLY | O_CLOEXEC |
                          O_DIRECTORY);
    if (fd < 0)
        return TError::System("Cannot open: " + path.ToString());
    ret = ioctl(fd, FS_IOC_FSGETXATTR, &attr);
    if (ret)
        saved_errno = errno;
    close(fd);
    if (ret)
        return TError(EError::Unknown, saved_errno, "Cannot get project quota: {}", path);
    id = attr.fsx_projid;
    return OK;
}

// Enable/disable inheritance from directory to files
TError TProjectQuota::Toggle(const TFile &dir, bool enabled) {
    struct fsxattr attr;

    if (ioctl(dir.Fd, FS_IOC_FSGETXATTR, &attr))
        return TError::System("ioctl FS_IOC_FSGETXATTR {}", dir.RealPath());

    if (enabled)
        attr.fsx_xflags |= FS_XFLAG_PROJINHERIT;
    else
        attr.fsx_xflags &= ~FS_XFLAG_PROJINHERIT;

    if (ioctl(dir.Fd, FS_IOC_FSSETXATTR, &attr))
        return TError::System("ioctl FS_IOC_FSSETXATTR {}", dir.RealPath());

    return OK;
}

TError TProjectQuota::SetProjectIdOne(const TPath &path, uint32_t id, bool isDir) {
    struct fsxattr attr;
    int fd, ret;
    TError error;

    fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW | O_NOATIME | O_NONBLOCK);
    if (fd < 0 && errno == EPERM)
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return TError::System("Cannot open: " + path.ToString());

    ret = ioctl(fd, FS_IOC_FSGETXATTR, &attr);
    if (!ret) {
        if (isDir)
            attr.fsx_xflags |= FS_XFLAG_PROJINHERIT;
        attr.fsx_projid = id;
        ret = ioctl(fd, FS_IOC_FSSETXATTR, &attr);
    }
    if (ret)
        error = TError::System("Cannot set project quota: {}", path);
    close(fd);
    return error;
}

TError TProjectQuota::SetProjectIdAll(const TPath &path, uint32_t id) {
    TPathWalk walk;
    TError error;

    error = walk.OpenNoStat(path);
    if (error)
        return error;

    while (1) {
        error = walk.Next();
        if (error || !walk.Path)
            return error;
        error = SetProjectIdOne(walk.Path, id, walk.Directory);
        if (error) {
            /* ignore errors for non-regular and non-directory */
            if (walk.Directory || walk.Path.IsRegularStrict())
                return error;
            L_VERBOSE("SetProjectIdAll {}", error);
        }
    }

    return OK;
}

/* Construct unique project id from directory inode number. */
TError TProjectQuota::InventProjectId(const TPath &path, uint32_t &id) {
    struct stat st;
    TError error;

    error = path.StatStrict(st);
    if (error)
        return error;

    id = st.st_ino | (1u << 31);
    return OK;
}

bool TProjectQuota::SeenInode(const struct stat *st) {
    return !Inodes.insert(static_cast<uint32_t>(st->st_ino)).second;
}

dqblk* TProjectQuota::FindQuota(uint32_t id) {
    auto quota = Quotas.find(id);
    if (quota != Quotas.end())
        return quota->second.get();
    else
        return nullptr;
}

dqblk* TProjectQuota::SearchQuota(uint32_t id) {
    dqblk *quota;
    quota = FindQuota(id);
    if (!quota) {
        quota = new dqblk();
        Quotas.emplace(id, std::unique_ptr<dqblk>(quota));
    }
    return quota;
}

TError TProjectQuota::WalkQuotaFile(int fd, unsigned id, int index, int depth) {
    __u32 block[256];
    int i;

    if (pread(fd, &block, sizeof(block),
              index * sizeof(block)) != sizeof(block))
        return TError::System("Cannot read quota block {}", index);

    id <<= 8;
    for (i = 0; i < 256; i++, id++) {
        if (!block[i])
            continue;
        if (depth == 3)
            SearchQuota(id);
        else
            WalkQuotaFile(fd, id, block[i], depth + 1);
    }
    return OK;
}

TError TProjectQuota::ScanQuotaFile(const TPath &quotaPath) {
    TError error;
    struct v2_disk_dqheader header;
    TFile quotaFile;

    quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);

    error = quotaFile.OpenRead(quotaPath);
    if (error)
        return error;
    if (read(quotaFile.Fd, &header, sizeof(header)) != sizeof(header))
        return TError::System("Cannot read quota file \"{}\"", quotaPath);
    if (header.dqh_magic != PROJECT_QUOTA_MAGIC)
        return TError(EError::InvalidValue, "Wrong quota file magic");
    if (header.dqh_version != 1)
        return TError(EError::NotSupported, "Unsupported quota file version");
    error = WalkQuotaFile(quotaFile.Fd, 0, 1, 0);
    return error;
}

TError TProjectQuota::WalkInodes(const TPathWalk &walk) {
    TError error;
    dqblk *quota;
    uint32_t id;

    if (SeenInode(walk.Stat))
        return OK;

    error = GetProjectId(walk.Path, id);
    if (error)
        return TError(EError::NotFound, "Cannot get project for file \"{}\": {}", walk.Path, error);

    quota = SearchQuota(id);
    if (quota) {
        quota->dqb_curinodes++;
        quota->dqb_curspace += walk.Stat->st_blocks << 9;
    }

    return OK;
}

TError TProjectQuota::WalkUnlinked() {
    TError error;
    size_t unlinkedInodes = 0;
    size_t unlinkedSpace = 0;
    struct dirent *ent;
    DIR *proc;
    uint64_t dev;

    dev = Path.GetDev();

    proc = opendir("/proc");
    if (!proc)
        return TError::System( "Cannot open \"/proc\"");

    while ((ent = readdir(proc))) {
        uint64_t fdSize;
        TFile file;
        pid_t pid;
        TPath path(fmt::format("/proc/{}/fd", ent->d_name));

        if (ent->d_type != DT_DIR)
            continue;

        pid = std::atoi(ent->d_name);
        if (pid <= 0)
            continue;

        error = GetFdSize(pid, fdSize);
        if (error) {
            L_WRN("{}", error.Text);
            continue;
        }

        error = file.OpenRead(path);
        if (error) {
            if (error.Errno != ENOENT)
                L_WRN("{}", error.Text);
            continue;
        }

        for (uint64_t fd = 0; fd < fdSize; fd++) {
            dqblk *quota;
            uint32_t id;
            struct stat st;
            TPath curPath(std::to_string(fd));

            error = file.StatAt(curPath, true, st);
            if (error) {
                if (error.Errno != ENOENT)
                    L_WRN("{}", error.Text);
                continue;
            }

            if (!S_ISREG(st.st_mode) || st.st_nlink || st.st_dev != dev)
                continue;

            if (SeenInode(&st))
                continue;

            if (GetProjectId(curPath, id)) {
                L_WRN("Cannot get project for file \"{}/{}\"", path, curPath);
                continue;
            }

            quota = FindQuota(id);
            if (quota) {
                unlinkedInodes++;
                unlinkedSpace += st.st_blocks << 9;

                L_WRN("Found unlinked inode for {}: \"{}/{}\" {} bytes",
                      id, path, curPath, st.st_blocks << 9);

                quota->dqb_curinodes++;
                quota->dqb_curspace += st.st_blocks << 9;
            }
        }
    }

    closedir(proc);

    if (unlinkedInodes)
        L_WRN("Found {} unlinked inodes, total {} bytes",
              unlinkedInodes, unlinkedSpace);

    return OK;
}

TError TProjectQuota::RecalcUsage() {
    TError error;
    TPathWalk walk;

    error = walk.OpenScan(Path);
    while (!error) {
        error = walk.Next();
        if (error || !walk.Path)
            break;
        error = WalkInodes(walk);
    }
    if (error)
        return error;

    error = WalkUnlinked();
    if (error)
        return error;

    return OK;
}

TError TProjectQuota::UpdateQuota(uint32_t id, const dqblk *realQuota, std::string &message) {
    dqblk quota;

    if (!id)
        return OK;

    if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), Device.c_str(), id, (caddr_t)&quota))
        return TError::System("Cannot get project quota \"{}\" at \"{}\"", id, Device.c_str());

    quota.dqb_valid = 0;

    auto addToMessage = [&message](const std::string &log) {
        message += log + "\n";
        L("{}", log);
    };

    if (quota.dqb_curinodes != realQuota->dqb_curinodes) {
        addToMessage(fmt::format("Update inode count for {}: {} -> {} ({})",
          id, quota.dqb_curinodes, realQuota->dqb_curinodes,
          (int64_t)(realQuota->dqb_curinodes - quota.dqb_curinodes)));

        quota.dqb_curinodes = realQuota->dqb_curinodes;
        quota.dqb_valid |= QIF_INODES;
    }

    if (quota.dqb_curspace != realQuota->dqb_curspace) {
        addToMessage(fmt::format("Update space usage for {}: {} -> {} ({})",
          id, quota.dqb_curspace, realQuota->dqb_curspace,
          (int64_t)(realQuota->dqb_curspace - quota.dqb_curspace)));

        quota.dqb_curspace = realQuota->dqb_curspace;
        quota.dqb_valid |= QIF_SPACE;
    }

    if (!realQuota->dqb_curinodes && !realQuota->dqb_curspace) {
        if (RemoveUnusedProjects) {
            addToMessage(fmt::format("Remove unused project quota: {}", id));
            memset(&quota, 0, sizeof(quota));
            quota.dqb_valid |= QIF_ALL;
        } else {
            L_WRN("Project {} seems unused", id);
        }
    }

    if (quota.dqb_valid) {
        if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA),
                     Device.c_str(), id, (caddr_t)&quota))
            return TError::System("Cannot set project quota \"{}\" at \"{}\"", id, Device.c_str());

        quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);
    }

    return OK;
}

TError TProjectQuota::FindProject() {
    TError error;

    error = GetProjectId(Path, ProjectId);
    if (error)
        return error;

    if (!ProjectId)
        return TError(EError::InvalidValue, "Project quota not found");

    uint32_t ExpectedId;
    error = InventProjectId(Path, ExpectedId);
    if (error)
        return error;

    if (ProjectId != ExpectedId)
        return TError(EError::InvalidValue, "Unexpected project quota: {} in {} expected {}",
                      Path, ProjectId, ExpectedId);

    return OK;
}

TError TProjectQuota::FindDevice() {
    TMount mount;
    TError error;

    /* already found */
    if (!Device.IsEmpty())
        return OK;

    auto device = Path.GetDev();
    if (!device)
        return TError("device not found: " + Path.ToString());

    std::vector<std::string> lines;
    error = TPath("/proc/self/mountinfo").ReadLines(lines, MOUNT_INFO_LIMIT);
    if (error)
        return error;

    /* find any writable non-bind mountpoint */
    //FIXME check overmounted mountpoints, for example via GetMountId
    for (auto &line : lines) {
        if (!mount.ParseMountinfo(line) &&
                device == mount.Device &&
                mount.BindPath.IsRoot() &&
                !(mount.MntFlags & MS_RDONLY)) {
            if (mount.Type != "ext4" && mount.Type != "xfs")
                return TError(EError::NotSupported, "Unsupported filesystem {}", mount.Type);
            Type = mount.Type;
            Device = mount.Source;
            RootPath = mount.Target;
            return OK;
        }
    }

    return TError("mountpoint not found: " + Path.ToString());
}

bool TProjectQuota::Exists() {
    uint32_t id;

    return !GetProjectId(Path, id) && id != 0;
}

TError TProjectQuota::Load() {
    dqblk quota;
    TError error;

    error = FindProject();
    if (error)
        return error;

    error = FindDevice();
    if (error)
        return error;

    if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), Device.c_str(),
                 ProjectId, (caddr_t)&quota))
        return TError::System("Cannot get quota state");

    SpaceLimit = quota.dqb_bhardlimit * QIF_DQBLKSIZE;
    SpaceUsage = quota.dqb_curspace;
    InodeLimit = quota.dqb_ihardlimit;
    InodeUsage = quota.dqb_curinodes;

    return OK;
}

TError TProjectQuota::Create() {
    dqblk quota;
    TError error;

    if (!Path.IsDirectoryStrict()) {
        if (!Path.Exists())
            return TError(EError::InvalidValue, "Directory not found: {}", Path);
        return TError(EError::InvalidValue, "Not a directory: {}", Path);
    }

    error = Enable();
    if (error)
        return error;

    error = InventProjectId(Path, ProjectId);
    if (error)
        return error;

    uint32_t CurrentId;
    error = GetProjectId(Path, CurrentId);
    if (error)
        return error;

    if (CurrentId && CurrentId != ProjectId)
        return TError(EError::Busy, "Path {} already in project quota {}", Path, CurrentId);

    if (!quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), Device.c_str(), ProjectId, (caddr_t)&quota)) {

        if ((quota.dqb_curinodes || quota.dqb_curspace) &&
                (!config().volumes().keep_project_quota_id() ||
                 CurrentId != ProjectId)) {

            L_WRN("Project quota {} for {} already in use: {} inodes {} bytes",
                    ProjectId, Path, quota.dqb_curinodes, quota.dqb_curspace);

            /* Reset quota counters */
            memset(&quota, 0, sizeof(quota));
            quota.dqb_valid = QIF_ALL;
            if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
                         ProjectId, (caddr_t)&quota))
                L_WRN("Cannot reset project quota {}: {}", ProjectId, TError::System(""));
        }
    } else if (errno != ENOENT)
        return TError::System("Cannot get quota state");

    memset(&quota, 0, sizeof(quota));
    quota.dqb_bhardlimit = SpaceLimit / QIF_DQBLKSIZE +
                !!(SpaceLimit % QIF_DQBLKSIZE);
    quota.dqb_ihardlimit = InodeLimit;
    quota.dqb_valid = QIF_LIMITS;

    if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
                 ProjectId, (caddr_t)&quota))
        return TError::System("Cannot set project quota {} limits", ProjectId);

    quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);

    /* Move files into project */
    if (CurrentId != ProjectId) {
        error = SetProjectIdAll(Path, ProjectId);
        if (error) {
            (void)SetProjectIdAll(Path, 0);
            (void)Destroy();
        }
    }

    return error;
}

TError TProjectQuota::Resize() {
    dqblk quota;
    TError error;

    error = FindProject();
    if (error)
        return error;

    error = FindDevice();
    if (error)
        return error;

    memset(&quota, 0, sizeof(quota));
    quota.dqb_bhardlimit = SpaceLimit / QIF_DQBLKSIZE +
                           !!(SpaceLimit % QIF_DQBLKSIZE);
    quota.dqb_ihardlimit = InodeLimit;
    quota.dqb_valid = QIF_LIMITS;

    if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
                 ProjectId, (caddr_t)&quota))
        return TError::System("Cannot set project quota {} limits", ProjectId);

    quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);
    return OK;
}

TError TProjectQuota::Destroy() {
    dqblk quota;
    TError error;

    error = FindProject();
    if (error)
        return error;

    error = FindDevice();
    if (error)
        return error;

    memset(&quota, 0, sizeof(quota));
    quota.dqb_valid = QIF_LIMITS;

    if (!config().volumes().keep_project_quota_id()) {
        error = SetProjectIdAll(Path, 0);
        if (!error && Type == "ext4")
            quota.dqb_valid = QIF_ALL;
    }

    if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
                 ProjectId, (caddr_t)&quota))
        L_WRN("Cannot set project quota {}: {}",
                ProjectId, TError::System(""));

    quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);

    return error;
}

TError TProjectQuota::Check(std::string &message) {
    TError error;

    error = FindDevice();
    if (error)
        return error;

    if (Path == RootPath) {
        /* scan all project id to remove unused */
        TPath quotaPath = RootPath / PROJECT_QUOTA_FILE;
        ScanQuotaFile(quotaPath);
        RemoveUnusedProjects = true;
    }

    error = RecalcUsage();
    if (error)
        return error;

    for (auto &dq: Quotas) {
        error = UpdateQuota(dq.first, dq.second.get(), message);
        if (error)
            return error;
    }
    return OK;
}

TError TProjectQuota::StatFS(TStatFS &result) {
    TError error;

    error = Path.StatFS(result);
    if (error)
        return error;

    error = Load();
    if (error)
        return error;

    result.SpaceUsage = SpaceUsage;

    if (SpaceLimit && SpaceLimit < SpaceUsage + result.SpaceAvail) {
        if (SpaceLimit > SpaceUsage)
            result.SpaceAvail = SpaceLimit - SpaceUsage;
        else
            result.SpaceAvail = 0;
    }

    result.InodeUsage = InodeUsage;

    if (InodeLimit && InodeLimit < InodeUsage + result.InodeAvail) {
        if (InodeLimit > InodeUsage)
            result.InodeAvail = InodeLimit - InodeUsage;
        else
            result.InodeAvail = 0;
    }

    return OK;
}
