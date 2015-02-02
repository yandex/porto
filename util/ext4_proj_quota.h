#ifndef __EXT4_PROJ_QUOTA_H__
#define __EXT4_PROJ_QUOTA_H__

#include <linux/quota.h>

extern int init_project_quota(const char *quota_path);
extern void project_quota_on(const char *path);
extern void project_quota_off(const char *path);
extern int get_project_id(const char *path, unsigned *project_id);
extern int set_project_id(const char *path, unsigned project_id);
extern void get_project_quota(const char *path, struct if_dqblk *quota);
extern void set_project_quota(const char *path, struct if_dqblk *quota);

#endif /* __EXT4_PROJ_QUOTA_H__ */
