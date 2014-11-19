#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>

#include "version.hpp"

const std::string ROOT_CONTAINER = "/";
const std::string PORTO_ROOT_CGROUP = "porto";
const int REAP_EVT_FD = 128;
const int REAP_ACK_FD = 129;

const std::string SYSFS_CGROOT = "/sys/fs/cgroup";

const std::string PORTO_STAT_SPAWNED = "/porto_spawned";
const std::string PORTO_STAT_ERRORS = "/porto_errors";
const std::string PORTO_STAT_WARNS = "/porto_warns";

#define NO_COPY_CONSTRUCT(NAME) \
    NAME(const NAME &) = delete; \
    NAME &operator=(const NAME &) = delete

#include "config.hpp"

#endif
