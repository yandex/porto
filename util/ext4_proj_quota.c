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

static int find_mountpoint(const char *path, struct stat *path_st,
			   char **device, char **fstype, char **root_path)
{
	struct stat dev_st;
	char *buf = NULL, *ptr, *real_device;
	unsigned major, minor;
	size_t len;
	FILE *file;

	if (lstat(path, path_st))
		return -1;

	*root_path = malloc(PATH_MAX + 1);
	if (!*root_path)
		return -1;

	/* since v2.6.26 */
	file = fopen("/proc/self/mountinfo", "r");
	if (!file)
		goto parse_mounts;
	while (getline(&buf, &len, file) > 0) {
		sscanf(buf, "%*d %*d %u:%u %*s %s", &major, &minor, *root_path);
		if (makedev(major, minor) != path_st->st_dev)
			continue;
		ptr = strstr(buf, " - ") + 3;
		*fstype = strdup(strsep(&ptr, " "));
		*device = strdup(strsep(&ptr, " "));
		goto found;
	}
	fclose(file);

parse_mounts:
	/* for older versions */
	file = fopen("/proc/mounts", "r");
	if (!file)
		goto not_found;
	while (getline(&buf, &len, file) > 0) {
		ptr = buf;
		strsep(&ptr, " ");
		if (*buf != '/' || stat(buf, &dev_st) ||
		    dev_st.st_rdev != path_st->st_dev)
			continue;
		strcpy(*root_path, strsep(&ptr, " "));
		*fstype = strdup(strsep(&ptr, " "));
		*device = strdup(buf);
		goto found;
	}
not_found:
	free(buf);
	free(*root_path);
	errno = ENODEV;
	return -1;

found:
	fclose(file);
	free(buf);
	if (!*fstype || !*device) {
		free(*fstype);
		free(*device);
		free(*root_path);
		errno = ENOMEM;
		return -1;
	}
	real_device = realpath(*device, NULL);
	if (real_device) {
		free(*device);
		*device = real_device;
	}
	return 0;
}

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

	fd = open(quota_path, O_CREAT|O_RDWR|O_EXCL, 0600);
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

	fd = open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_NOATIME | O_NONBLOCK);
	if (fd < 0 && errno == EPERM)
		fd = open(path, O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK);
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

	if (mount(NULL, root_path, NULL, MS_REMOUNT, "quota")) {
		warn("Cannot enable project quota in \"%s\"", root_path);
		return -1;
	}

	if (asprintf(&quota_path, "%s/%s", root_path, PROJECT_QUOTA_FILE) < 0)
		return -1;

	if (access(quota_path, F_OK) && init_project_quota(quota_path)) {
		warn("Cannot init project quota file \"%s\"", quota_path);
		free(quota_path);
		return -1;
	}

	if (quotactl(QCMD(Q_QUOTAON, PRJQUOTA), device,
				QFMT_VFS_V1, (caddr_t)quota_path)) {
		warn("Cannot turn on project quota for %s", device);
		free(quota_path);
		return -1;
	}

	free(quota_path);
	return 0;
}

static unsigned target_project;

static int walk_set_project(const char *path, const struct stat *st,
				int flag, struct FTW *data)
{
	int ret;

	(void)st;
	(void)flag;
	(void)data;

	if (flag == FTW_NS) {
		warnx("Cannot stat file \"%s\". Aborting.", path);
		return -1;
	}

	if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)) {
		warnx("Impossible set project for non-regular file \"%s\". Skipping.", path);
		return 0;
	}

	ret = set_project(path, target_project);
	if (ret)
		warn("Cannot set project for \"%s\". Aborting.", path);

	return ret;
}

int ext4_support_project(const char *path)
{
	char *device, *fstype, *root_path;
	struct stat path_st;
	struct if_dqinfo dqinfo;
	int ret = -1;

	if (find_mountpoint(path, &path_st, &device, &fstype, &root_path))
		goto out;

	if (strcmp(fstype, "ext4"))
		goto out;

	if (quotactl(QCMD(Q_GETINFO, PRJQUOTA), device, 0, (caddr_t)&dqinfo) &&
	    (errno != ESRCH || project_quota_on(device, root_path)))
		goto out;

	ret = 0;
out:
	free(device);
	free(fstype);
	free(root_path);

	return ret;
}

int ext4_create_project(const char *path,
			unsigned long long max_bytes,
			unsigned long long max_inodes)
{
	char *device, *fstype, *root_path;
	struct if_dqblk quota;
	struct stat path_st;
	struct if_dqinfo dqinfo;
	unsigned project;
	int ret = -1;

	if (find_mountpoint(path, &path_st, &device, &fstype, &root_path)) {
		warn("Cannot find mountpoint for \"%s\"", path);
		return -1;
	}

	if (strcmp(fstype, "ext4")) {
		warnx("Unsupported filesystem \"%s\"", fstype);
		goto out;
	}

	if (!S_ISDIR(path_st.st_mode)) {
		warn("Project root must be a directory: \"%s\"", path);
		goto out;
	}

	if (quotactl(QCMD(Q_GETINFO, PRJQUOTA), device, 0, (caddr_t)&dqinfo) &&
	    project_quota_on(device, root_path))
		goto out;

	project = path_st.st_ino | (1u << 31);

	if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota)) {
		warn("Cannot get project quota %u", project);
		goto out;
	}

	if (quota.dqb_curspace || quota.dqb_curinodes)
		warn("Project %u inuse or stale. Reinitializing.", project);

	memset(&quota, 0, sizeof(quota));
	quota.dqb_bhardlimit = (max_bytes + QIF_DQBLKSIZE - 1) / QIF_DQBLKSIZE;
	quota.dqb_ihardlimit = max_inodes;
	quota.dqb_valid = QIF_ALL;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota)) {
		warn("Cannot set project quota %u", project);
		goto out;
	}
	quotactl(QCMD(Q_SYNC, PRJQUOTA), device, 0, NULL);

	target_project = project;
	ret = nftw(path, walk_set_project, 100, FTW_PHYS | FTW_MOUNT);
out:
	free(root_path);
	free(fstype);
	free(device);
	return ret;
}

extern int ext4_resize_project(const char *path,
			       unsigned long long max_bytes,
			       unsigned long long max_inodes)
{
	char *device, *fstype, *root_path;
	struct if_dqblk quota;
	struct stat path_st;
	unsigned project;
	int ret = -1;

	if (find_mountpoint(path, &path_st, &device, &fstype, &root_path)) {
		warn("cannot find mountpoint for \"%s\"", path);
		return -1;
	}

	if (get_project(path, &project)) {
		warn("Cannot get project for path \"%s\"", path);
		goto out;
	}

	memset(&quota, 0, sizeof(quota));
	quota.dqb_bhardlimit = (max_bytes + QIF_DQBLKSIZE - 1)  / QIF_DQBLKSIZE;
	quota.dqb_ihardlimit = max_inodes;
	quota.dqb_valid = QIF_LIMITS;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota)) {
		warn("Cannot set project quota %u", project);
		goto out;
	}
	quotactl(QCMD(Q_SYNC, PRJQUOTA), device, 0, NULL);

	ret = 0;
out:
	free(root_path);
	free(fstype);
	free(device);
	return ret;
}

int ext4_destroy_project(const char *path)
{
	char *device, *fstype, *root_path;
	struct if_dqblk quota;
	struct stat path_st;
	struct if_dqinfo dqinfo;
	unsigned project;
	int ret = -1;

	if (find_mountpoint(path, &path_st, &device, &fstype, &root_path)) {
		warn("cannot find mountpoint for \"%s\"", path);
		return -1;
	}

	if (strcmp(fstype, "ext4")) {
		warnx("Unsupported filesystem \"%s\"", fstype);
		goto out;
	}

	if (!S_ISDIR(path_st.st_mode)) {
		warnx("Project root must be a directory: \"%s\"", path);
		goto out;
	}

	if (get_project(path, &project)) {
		warn("Cannot get project for directory: \"%s\"", path);
		goto out;
	}

	if (project != (path_st.st_ino | (1u << 31))) {
		warnx("Not a project root: \"%s\". Cannot destroy it.", path);
		goto out;
	}

	target_project = 0;
	ret = nftw(path, walk_set_project, 100, FTW_PHYS | FTW_MOUNT);

	memset(&quota, 0, sizeof(quota));
	quota.dqb_valid = QIF_ALL;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project, (caddr_t)&quota)) {
		warn("Cannot clear project %u quota", project);
		ret = -1;
	}
	quotactl(QCMD(Q_SYNC, PRJQUOTA), device, 0, NULL);
out:
	free(root_path);
	free(fstype);
	free(device);
	return ret;
}
