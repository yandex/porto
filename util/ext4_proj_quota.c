#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>
#include <ftw.h>
#include <linux/quota.h>
#include <sys/quota.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "ext4_proj_quota.h"

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

#define FS_IOC_FSGETXATTR		_IOR('X', 31, struct fsxattr)
#define FS_IOC_FSSETXATTR		_IOW('X', 32, struct fsxattr)

#define FS_XFLAG_PROJINHERIT		0x00000200 /* Ignored */

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

#define PROJECT_QUOTA_FILE	"quota.project"
#define PROJECT_QUOTA_MAGIC	0xd9c03f14

static int init_project_quota(const char *quota_path)
{
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

	fd = open(quota_path, O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return fd;
	ret = write(fd, &quota_init, sizeof(quota_init));
	saved_errno = errno;
	fsync(fd);
	close(fd);
	if (ret == sizeof(quota_init))
		return 0;
	if (ret >= 0)
		saved_errno = EIO;
	errno = saved_errno;
	return -1;
}

static int get_project(const char *path, unsigned *project)
{
	struct fsxattr attr;
	int fd, ret, saved_errno;

	fd = open(path, O_CLOEXEC | O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_NOATIME | O_NONBLOCK);
	if (fd < 0 && errno == EPERM)
		fd = open(path, O_CLOEXEC | O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		char *dirc, *dir;

		dirc = strdup(path);
		dir = dirname(dirc);
		fd = open(dir, O_RDONLY | O_DIRECTORY);
		free(dirc);
	}
	if (fd < 0)
		return fd;
	ret = ioctl(fd, FS_IOC_FSGETXATTR, &attr);
	if (ret)
		saved_errno = errno;
	close(fd);
	if (ret)
		errno = saved_errno;
	*project = attr.fsx_projid;
	return ret;
}

static int set_project(const char *path, unsigned project)
{
	int fd, ret, saved_errno;
	struct fsxattr attr;

	fd = open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_NOATIME | O_NONBLOCK);
	if (fd < 0 && errno == EPERM)
		fd = open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return fd;

	ret = ioctl(fd, FS_IOC_FSGETXATTR, &attr);
	if (!ret) {
		attr.fsx_xflags |= FS_XFLAG_PROJINHERIT;
		attr.fsx_projid = project;
		ret = ioctl(fd, FS_IOC_FSSETXATTR, &attr);
	}
	if (ret)
		saved_errno = errno;
	close(fd);
	if (ret)
		errno = saved_errno;
	return ret;
}

static int project_quota_on(const char *device, const char *root_path)
{
	char *quota_path;
	int ret;

	ret = mount(NULL, root_path, NULL, MS_REMOUNT, "quota");
	if (ret)
		return ret;

	ret = asprintf(&quota_path, "%s/%s", root_path, PROJECT_QUOTA_FILE);
	if (ret < 0)
		return ret;

	if (access(quota_path, F_OK)) {
		ret = init_project_quota(quota_path);
		if (ret)
			goto out;
	}

	ret = quotactl(QCMD(Q_QUOTAON, PRJQUOTA), device,
		       QFMT_VFS_V1, (caddr_t)quota_path);
out:
	free(quota_path);
	return ret;
}

static unsigned target_project;

static int walk_set_project(const char *path, const struct stat *st,
				int flag, struct FTW *data)
{
	(void)st;
	(void)flag;
	(void)data;

	if (flag == FTW_NS)
		return -1;
	if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode))
		return 0;
	return set_project(path, target_project);
}

int ext4_support_project(const char *device,
			 const char *fstype,
			 const char *root_path)
{
	struct if_dqinfo dqinfo;

	if (strcmp(fstype, "ext4")) {
		errno = ENOTSUP;
		return -1;
	}

	if (quotactl(QCMD(Q_GETINFO, PRJQUOTA), device, 0, (caddr_t)&dqinfo) &&
			errno == ESRCH)
		return project_quota_on(device, root_path);
	return 0;
}

int ext4_create_project(const char *device,
			const char *path,
			unsigned long long max_bytes,
			unsigned long long max_inodes)
{
	struct if_dqblk quota;
	struct stat path_st;
	struct if_dqinfo dqinfo;
	unsigned project;

	if (lstat(path, &path_st))
		return -1;

	if (!S_ISDIR(path_st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	project = path_st.st_ino | (1u << 31);

	if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota))
		return -1;

	memset(&quota, 0, sizeof(quota));
	quota.dqb_bhardlimit = (max_bytes + QIF_DQBLKSIZE - 1) / QIF_DQBLKSIZE;
	quota.dqb_ihardlimit = max_inodes;
	quota.dqb_valid = QIF_ALL;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota))
		return -1;
	quotactl(QCMD(Q_SYNC, PRJQUOTA), device, 0, NULL);

	target_project = project;
	return nftw(path, walk_set_project, 100, FTW_PHYS | FTW_MOUNT);
}

int ext4_resize_project(const char *device,
			const char *path,
			unsigned long long max_bytes,
			unsigned long long max_inodes)
{
	struct if_dqblk quota;
	unsigned project;

	if (get_project(path, &project))
		return -1;

	memset(&quota, 0, sizeof(quota));
	quota.dqb_bhardlimit = (max_bytes + QIF_DQBLKSIZE - 1)  / QIF_DQBLKSIZE;
	quota.dqb_ihardlimit = max_inodes;
	quota.dqb_valid = QIF_LIMITS;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota))
		return -1;
	quotactl(QCMD(Q_SYNC, PRJQUOTA), device, 0, NULL);
	return 0;
}

int ext4_destroy_project(const char *device, const char *path)
{
	struct if_dqblk quota;
	struct stat path_st;
	struct if_dqinfo dqinfo;
	unsigned project;

	if (lstat(path, &path_st))
		return -1;

	if (!S_ISDIR(path_st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	if (get_project(path, &project))
		return -1;

	if (project != (path_st.st_ino | (1u << 31))) {
		errno = ENOTDIR;
		return -1;
	}

	target_project = 0;
	(void)nftw(path, walk_set_project, 100, FTW_PHYS | FTW_MOUNT);

	memset(&quota, 0, sizeof(quota));
	quota.dqb_valid = QIF_ALL;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota))
		return -1;
	quotactl(QCMD(Q_SYNC, PRJQUOTA), device, 0, NULL);
	return 0;
}
