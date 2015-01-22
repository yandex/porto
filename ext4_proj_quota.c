#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/quota.h>
#include <sys/quota.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#ifndef PRJQUOTA
#define PRJQUOTA 2
#endif

#define FS_IOC_FSGETXATTR		_IOR('f', 31, struct fsxattr)
#define FS_IOC_FSSETXATTR		_IOW('f', 32, struct fsxattr)

/*
 * Structure for FS_IOC_FSGETXATTR and FS_IOC_FSSETXATTR.
 */
struct fsxattr {
	__u32		fsx_xflags;	/* xflags field value (get/set) */
	__u32		fsx_extsize;	/* extsize field value (get/set)*/
	__u32		fsx_nextents;	/* nextents field value (get)	*/
	__u32		fsx_projid;	/* project identifier (get/set) */
	unsigned char	fsx_pad[12];
};

#define FS_XFLAG_PROJINHERIT	0x00000200	/* create with parents projid */

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

static int find_mountpoint(const char *path,
		char **device, char **fstype, char **root_path)
{
	struct stat path_st, dev_st;
	char *buf = NULL, *ptr, *real_device;
	unsigned major, minor;
	size_t len;
	FILE *file;

	if (stat(path, &path_st))
		return -1;

	*root_path = malloc(PATH_MAX + 1);

	/* since v2.6.26 */
	file = fopen("/proc/self/mountinfo", "r");
	if (!file)
		goto parse_mounts;
	while (getline(&buf, &len, file) > 0) {
		sscanf(buf, "%*d %*d %u:%u %*s %s", &major, &minor, *root_path);
		if (makedev(major, minor) != path_st.st_dev)
			continue;
		ptr = strstr(buf, " - ") + 3;
		*fstype = strdup(strsep(&ptr, " "));
		*device = strdup(strsep(&ptr, " "));
		goto found;
	}

parse_mounts:
	/* for older versions */
	file = fopen("/proc/mounts", "r");
	if (!file)
		goto not_found;
	while (getline(&buf, &len, file) > 0) {
		ptr = buf;
		strsep(&ptr, " ");
		if (*buf != '/' || stat(buf, &dev_st) ||
		    dev_st.st_rdev != path_st.st_dev)
			continue;
		strcpy(*root_path, strsep(&ptr, " "));
		*fstype = strdup(strsep(&ptr, " "));
		*device = strdup(buf);
		goto found;
	}
not_found:
	free(*root_path);
	errno = ENODEV;
	return -1;

found:
	real_device = realpath(*device, NULL);
	if (real_device) {
		free(*device);
		*device = real_device;
	}
	return 0;
}

static int init_project_quota(const char *quota_path)
{
	FILE *quota_file;
	struct {
		struct v2_disk_dqheader header;
		struct v2_disk_dqinfo info;
		char zero[1024 - 8*4];
	} quota_init = {
		.header = {
			.dqh_magic = 0xd9c03f14,
			.dqh_version = 1,
		},
		.info = {
			.dqi_bgrace = 7 * 24 * 60 * 60,
			.dqi_igrace = 7 * 24 * 60 * 60,
			.dqi_flags = 0,
			.dqi_blocks = 1,
			.dqi_free_blk = 0,
			.dqi_free_entry = 0,
		},
		.zero = {0, },
	};

	quota_file = fopen(quota_path, "wx");
	if (!quota_file)
		return -1;
	fwrite(&quota_init, sizeof(quota_init), 1, quota_file);
	fflush(quota_file);
	fclose(quota_file);
	return 0;
}

static void project_quota_on(const char *path)
{
	char *device, *fstype ,*root_path, *quota_path;

	if (find_mountpoint(path, &device, &fstype, &root_path))
		err(2, "cannot find mountpoint for \"%s\"", path);

	if (strcmp(fstype, "ext4"))
		errx(2, "unsupported filesystem \"%s\"", fstype);

	if (mount(NULL, root_path, NULL, MS_REMOUNT, "prjquota"))
		err(2, "cannot enable project quota in \"%s\"", root_path);

	asprintf(&quota_path, "%s/%s", root_path, PROJECT_QUOTA_FILE);
	if (access(quota_path, F_OK) && init_project_quota(quota_path))
		err(2, "cannot init project quota file \"%s\"", quota_path);

	if (quotactl(QCMD(Q_QUOTAON, PRJQUOTA), device,
				QFMT_VFS_V1, (caddr_t)quota_path))
		err(2, "cannot turn on project quota for %s", device);

	free(device);
	free(fstype);
	free(root_path);
	free(quota_path);
}

static void project_quota_off(const char *path)
{
	char *device, *fstype ,*root_path;

	if (find_mountpoint(path, &device, &fstype, &root_path))
		err(2, "cannot find mountpoint for \"%s\"", path);

	if (strcmp(fstype, "ext4"))
		errx(2, "unsupported filesystem \"%s\"", fstype);

	if (quotactl(QCMD(Q_QUOTAOFF, PRJQUOTA), device, 0, NULL))
		err(2, "cannot turn off project quota for %s", device);

	//if (mount(NULL, root_path, NULL, MS_REMOUNT, "noquota"))
	//	err(2, "cannot disable project quota in \"%s\"", root_path);

	free(device);
	free(fstype);
	free(root_path);
}

static int get_project_id(const char *path, unsigned *project_id)
{
	struct fsxattr fsx;
	int dir_fd, ret;

	dir_fd = open(path, O_RDONLY | O_NOCTTY);
	if (dir_fd < 0)
		return dir_fd;
	ret = ioctl(dir_fd, FS_IOC_FSGETXATTR, &fsx);
	if (!ret)
		*project_id = fsx.fsx_projid;
	close(dir_fd);
	return ret;
}

static int set_project_id(const char *path, unsigned project_id)
{
	struct fsxattr fsx;
	int dir_fd, ret;

	dir_fd = open(path, O_RDONLY | O_NOCTTY);
	if (dir_fd < 0)
		return dir_fd;

	ret = ioctl(dir_fd, FS_IOC_FSGETXATTR, &fsx);
	if (!ret) {
		fsx.fsx_xflags |= FS_XFLAG_PROJINHERIT;
		fsx.fsx_projid = project_id;
		ret = ioctl(dir_fd, FS_IOC_FSSETXATTR, &fsx);
	}
	close(dir_fd);
	return ret;
}

static void get_project_quota(const char *path, struct if_dqblk *quota)
{
	char *root_path, *fstype, *device;
	unsigned project_id;

	if (find_mountpoint(path, &device, &fstype, &root_path))
		err(2, "cannot find mountpoint for \"%s\"", path);

	if (strcmp(fstype, "ext4"))
		errx(2, "unsupported filesystem \"%s\"", fstype);

	if (get_project_id(path, &project_id))
		err(2, "cannot get project id for \"%s\"", path);

	if (!project_id)
		errx(2, "project id isn't set for \"%s\"", path);

	if (quotactl(QCMD(Q_GETQUOTA, PRJQUOTA), device,
				project_id, (caddr_t)quota))
		err(2, "cannot get project quota \"%u\" at \"%s\"",
				project_id, root_path);

	free(root_path);
	free(fstype);
	free(device);
}

static void set_project_quota(const char *path, struct if_dqblk *quota)
{
	char *root_path, *fstype, *device;
	unsigned project_id;

	if (find_mountpoint(path, &device, &fstype, &root_path))
		err(2, "cannot find mountpoint for \"%s\"", path);

	if (strcmp(fstype, "ext4"))
		errx(2, "unsupported filesystem \"%s\"", fstype);

	if (get_project_id(path, &project_id))
		err(2, "cannot get project id for \"%s\"", path);

	if (!project_id)
		errx(2, "project id isn't set for \"%s\"", path);

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device,
				project_id, (caddr_t)quota))
		err(2, "cannot set project quota limit \"%u\" at \"%s\"",
				project_id, root_path);

	free(root_path);
	free(fstype);
	free(device);
}

int main (int argc, char **argv) {
	struct if_dqblk quota;
	unsigned project_id;

	if (argc < 2)
		goto usage;

	if (!strcmp(argv[1], "on")) {
		if (argc < 3)
			goto usage;
		project_quota_on(argv[2]);
	} else if (!strcmp(argv[1], "off")) {
		if (argc < 3)
			goto usage;
		project_quota_off(argv[2]);
	} else if (!strcmp(argv[1], "project")) {
		if (argc < 4)
			goto usage;
		set_project_id(argv[2], atoi(argv[3]));
	} else if (!strcmp(argv[1], "limit")) {
		if (argc < 4)
			goto usage;
		quota.dqb_bhardlimit = atoll(argv[3]) / QIF_DQBLKSIZE;
		quota.dqb_bsoftlimit = quota.dqb_bhardlimit;
		quota.dqb_valid |= QIF_BLIMITS;
		set_project_quota(argv[2], &quota);
	} else if (!strcmp(argv[1], "ilimit")) {
		if (argc < 4)
			goto usage;
		quota.dqb_ihardlimit = atoll(argv[3]);
		quota.dqb_isoftlimit = quota.dqb_bhardlimit;
		quota.dqb_valid |= QIF_ILIMITS;
		set_project_quota(argv[2], &quota);
	} else if (!strcmp(argv[1], "info")) {
		if (argc < 3)
			goto usage;
		get_project_id(argv[2], &project_id);
		get_project_quota(argv[2], &quota);
		printf("project   %u\n", project_id);
		printf("space     %llu\n", quota.dqb_curspace);
		printf("limit     %llu\n", quota.dqb_bhardlimit * QIF_DQBLKSIZE);
		printf("inodes    %llu\n", quota.dqb_curinodes);
		printf("ilimit    %llu\n", quota.dqb_ihardlimit);
	}

	return 0;

usage:
	fprintf(stderr, "Usage: %s <command> <path> [args]...\n"
			"Commands: \n"
			"  on      <path>                turn on\n"
			"  off     <path>                turn off\n"
			"  info    <path>                print usage and limits\n"
			"  project <path> <id>           set project id\n"
			"  limit   <path> <bytes>        set space limit\n"
			"  ilimit  <path> <inodes>       set inodes limit\n",
			argv[0]);
	return 2;
}
