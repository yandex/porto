#include "quota.hpp"
#include "mount.hpp"

extern "C" {
#include <linux/quota.h>
#include <sys/quota.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <fts.h>
}

#ifndef PRJQUOTA
#define PRJQUOTA 2
#endif

struct fsxattr {
	__u32		fsx_xflags;
	__u32		fsx_extsize;
	__u32		fsx_nextents;
	__u32		fsx_projid;
	unsigned char	fsx_pad[12];
};

#ifndef FS_IOC_FSGETXATTR
#define FS_IOC_FSGETXATTR		_IOR('X', 31, struct fsxattr)
#endif

#ifndef FS_IOC_FSSETXATTR
#define FS_IOC_FSSETXATTR		_IOW('X', 32, struct fsxattr)
#endif

#ifndef FS_XFLAG_PROJINHERIT
#define FS_XFLAG_PROJINHERIT		0x00000200
#endif

/* First generic header */
struct v2_disk_dqheader {
	__le32 dqh_magic;	/* Magic number identifying file */
	__le32 dqh_version;	/* File version */
};

/* Header with type and version specific information */
struct v2_disk_dqinfo {
	__le32 dqi_bgrace;	/* Time before block soft limit becomes hard limit */
	__le32 dqi_igrace;	/* Time before inode soft limit becomes hard limit */
	__le32 dqi_flags;	/* Flags for quotafile (DQF_*) */
	__le32 dqi_blocks;	/* Number of blocks in file */
	__le32 dqi_free_blk;	/* Number of first free block in the list */
	__le32 dqi_free_entry;	/* Number of block with at least one free entry */
};

TError TProjectQuota::InitProjectQuotaFile(TPath path) {
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
		return TError(EError::Unknown, errno, "Cannot create quota file");
	ret = write(fd, &quota_init, sizeof(quota_init));
	saved_errno = errno;
	fsync(fd);
	close(fd);
	if (ret == sizeof(quota_init))
		return TError::Success();
	if (ret >= 0)
		saved_errno = EIO;
	return TError(EError::Unknown, saved_errno, "Cannot write quota file");
}

TError TProjectQuota::EnableProjectQuota() {
	struct if_dqinfo dqinfo;
	TError error;
	int ret;

	if (quotactl(QCMD(Q_GETINFO, PRJQUOTA),
		     Device.c_str(), 0, (caddr_t)&dqinfo) == 0)
		return TError::Success();

	ret = mount(NULL, RootPath.c_str(), NULL, MS_REMOUNT, "quota");
	if (ret)
		TError(EError::Unknown, errno, "Cannot enable quota");

	TPath quota = RootPath / PROJECT_QUOTA_FILE;
	if (!quota.Exists()) {
		error = InitProjectQuotaFile(quota);
		if (error)
			return error;
	}

	ret = quotactl(QCMD(Q_QUOTAON, PRJQUOTA), Device.c_str(),
		       QFMT_VFS_V1, (caddr_t)quota.c_str());
	if (ret)
		error = TError(EError::Unknown, errno, "Cannot enable quota");

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
		fd = open(path.DirName().c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return TError(EError::Unknown, errno, "Cannot open: " + path.ToString());
	ret = ioctl(fd, FS_IOC_FSGETXATTR, &attr);
	if (ret)
		saved_errno = errno;
	close(fd);
	if (ret)
		return TError(EError::Unknown, saved_errno, "Cannot get quota id: " + path.ToString());
	id = attr.fsx_projid;
	return TError::Success();
}

TError TProjectQuota::SetProjectIdOne(const char *path, uint32_t id) {
	struct fsxattr attr;
	int fd, ret;
	TError error;

	fd = open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW |
				O_NOATIME | O_NONBLOCK);
	if (fd < 0 && errno == EPERM)
		fd = open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW |
					O_NONBLOCK);
	if (fd < 0)
		return TError(EError::Unknown, errno, "Cannot open: " + std::string(path));

	ret = ioctl(fd, FS_IOC_FSGETXATTR, &attr);
	if (!ret) {
		attr.fsx_xflags |= FS_XFLAG_PROJINHERIT;
		attr.fsx_projid = id;
		ret = ioctl(fd, FS_IOC_FSSETXATTR, &attr);
	}
	if (ret)
		error = TError(EError::Unknown, errno, "Cannot set quota id: " + std::string(path));
	close(fd);
	return error;
}

TError TProjectQuota::SetProjectIdAll(const TPath &path, uint32_t id) {
	const char *paths[] = { path.c_str(), NULL };
	TError error;

	FTS *fts = fts_open((char**)paths, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, NULL);
	if (!fts)
		return TError(EError::Unknown, errno, "fts_open");

	do {
		FTSENT *ent = fts_read(fts);
		if (ent == NULL) {
			if (errno)
				error = TError(EError::Unknown, errno, "fts_read");
			break;
		}
		switch (ent->fts_info) {
			case FTS_D:
			case FTS_F:
				error = SetProjectIdOne(ent->fts_accpath, id);
				break;
			case FTS_DC:
			case FTS_NS:
			case FTS_DNR:
			case FTS_ERR:
			case FTS_NSOK:
				error = TError(EError::Unknown, errno,
						"SetProjectId walk error: " +
						std::string(ent->fts_accpath));
				break;
			case FTS_DP:
			case FTS_SL:
			case FTS_SLNONE:
			case FTS_DEFAULT:
			case FTS_DOT:
				break;
		}
	} while (!error);
	fts_close(fts);
	return error;
}

/* Construct unique project id from directory inode number. */
TError TProjectQuota::InventProjectId(const TPath &path, uint32_t &id) {
	id = path.GetInode();
	if (!id)
		return TError(EError::Unknown, "Cannot get inode number: " + path.ToString());
	id |= 1u << 31;
	return TError::Success();
}

TError TProjectQuota::FindDevice() {
	TMount mount;
	TError error;

	error = mount.Find(Path);
	if (error)
		return error;

	if (mount.GetType() != "ext4")
		return TError(EError::NotSupported,
				"Unsupported filesystem " + mount.GetType());

	Device = mount.GetSource();
	RootPath = mount.GetMountpoint();
	return TError::Success();
}

bool TProjectQuota::Supported() {
	if (Device.IsEmpty() && FindDevice())
		return false;
	if (EnableProjectQuota())
		return false;
	return true;
}

bool TProjectQuota::Exists() {
	uint32_t id;

	return !GetProjectId(Path, id) && id != 0;
}

TError TProjectQuota::Load() {
	struct if_dqblk quota;
	TError error;

	if (!ProjectId) {
		error = GetProjectId(Path, ProjectId);
		if (error)
			return error;
		if (!ProjectId)
			return TError(EError::InvalidValue, "Project id not found");

		uint32_t ExpectedId;
		error = InventProjectId(Path, ExpectedId);
		if (error)
			return error;
		if (ProjectId != ExpectedId)
			return TError(EError::InvalidValue, "Project id not match with inode: " + Path.ToString() +
					" found: " + std::to_string(ProjectId) +
					" expected: " + std::to_string(ExpectedId));
	}

	if (Device.IsEmpty()) {
		error = FindDevice();
		if (error)
			return error;
	}

	if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), Device.c_str(),
				ProjectId, (caddr_t)&quota))
		return TError(EError::Unknown, "Cannot get quota state");

	SpaceLimit = quota.dqb_bhardlimit * QIF_DQBLKSIZE;
	SpaceUsage = quota.dqb_curspace;
	InodeLimit = quota.dqb_ihardlimit;
	InodeUsage = quota.dqb_curinodes;

	return TError::Success();
}

TError TProjectQuota::Create() {
	struct if_dqblk quota;
	TError error;
	TMount mount;

	if (!Path.IsDirectory())
		return TError(EError::InvalidValue, "Not a directory " + Path.ToString());

	if (Device.IsEmpty()) {
		error = FindDevice();
		if (error)
			return error;
	}

	error = EnableProjectQuota();
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
		return TError(EError::Unknown, "Cannot create nested project quota: " +
				Path.ToString() + " already in project " +
				std::to_string(CurrentId));

	if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), Device.c_str(),
				ProjectId, (caddr_t)&quota))
		return TError(EError::Unknown, "Cannot get quota state");

	memset(&quota, 0, sizeof(quota));
	quota.dqb_bhardlimit = SpaceLimit / QIF_DQBLKSIZE;
	quota.dqb_ihardlimit = InodeLimit;
	quota.dqb_valid = QIF_ALL;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
				ProjectId, (caddr_t)&quota))
		return TError(EError::Unknown, "Cannot set quota state");

	quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);

	error = SetProjectIdAll(Path, ProjectId);
	if (error)
		(void)Destroy();

	return error;
}

TError TProjectQuota::Resize() {
	struct if_dqblk quota;
	TError error;

	if (Device.IsEmpty()) {
		error = Load();
		if (error)
			return error;
	}

	memset(&quota, 0, sizeof(quota));
	quota.dqb_bhardlimit = SpaceLimit / QIF_DQBLKSIZE;
	quota.dqb_ihardlimit = InodeLimit;
	quota.dqb_valid = QIF_LIMITS;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
				ProjectId, (caddr_t)&quota))
		return TError(EError::Unknown, "Cannot set quota state");
	quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);
	return TError::Success();
}

TError TProjectQuota::Destroy() {
	struct if_dqblk quota;
	TError error;

	if (Device.IsEmpty()) {
		error = Load();
		if (error)
			return error;
	}

	error = SetProjectIdAll(Path, 0);
	if (error)
		return error;

	memset(&quota, 0, sizeof(quota));
	quota.dqb_valid = QIF_ALL;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), Device.c_str(),
				ProjectId, (caddr_t)&quota))
		return TError(EError::Unknown, "Cannot set quota state");
	quotactl(QCMD(Q_SYNC, PRJQUOTA), Device.c_str(), 0, NULL);
	return TError::Success();
}
