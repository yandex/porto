#pragma once

#include "util/error.hpp"

#define __STDC_LIMIT_MACROS
#include <cstdint>
#undef __STDC_LIMIT_MACROS

#define noinline __attribute__((noinline))

#define BIT(nr) (1ULL << (nr))

#ifndef __has_feature
# define __has_feature(__x) 0
#endif

class TNonCopyable {
protected:
    TNonCopyable() = default;
    ~TNonCopyable() = default;
private:
    TNonCopyable(TNonCopyable const&) = delete;
    TNonCopyable& operator= (TNonCopyable const&) = delete;
    TNonCopyable(TNonCopyable const&&) = delete;
    TNonCopyable& operator= (TNonCopyable const&&) = delete;
};

enum class EAccessLevel {
    None,
    ReadIsolate,
    ReadOnly,
    Isolate,
    SelfIsolate, /* allows to change self limits */
    ChildOnly,
    Normal,
    SuperUser,
    Internal,
};

constexpr uint32_t ROOT_TC_MAJOR = 0x1;
constexpr uint32_t META_TC_MINOR = 0x8;
constexpr uint32_t ROOT_TC_MINOR = 0x10;
constexpr uint32_t DEFAULT_TC_MINOR = 0x20; /* fallback class */
constexpr int NR_TC_CLASSES = 8;

constexpr uint64_t CPU_POWER_PER_SEC = 1000000000ull;

constexpr uint64_t NSEC_PER_SEC = 1000000000ull;

constexpr uint64_t NET_MAX_RATE = 12500000000000ull; /* 100Tbit */

constexpr int ROOT_CONTAINER_ID = 1;
constexpr int DEFAULT_CONTAINER_ID = 2;
constexpr int LEGACY_CONTAINER_ID = 3;
constexpr int CONTAINER_ID_MAX = 4095;
constexpr int CONTAINER_LEVEL_MAX = 16;

constexpr const char *ROOT_CONTAINER = "/";
constexpr const char *ROOT_PORTO_NAMESPACE = "/porto/";
constexpr const char *PORTO_CGROUP_PREFIX = "/porto";

constexpr const char *DOT_CONTAINER = ".";
constexpr const char *SELF_CONTAINER = "self";

constexpr int NR_SERVICE_CONTAINERS = 1;

constexpr int NR_SUPERUSER_CLIENTS = 100;
constexpr int NR_SUPERUSER_CONTAINERS = 100;
constexpr int NR_SUPERUSER_VOLUMES = 100;

constexpr const char *PORTO_DAEMON_CGROUP = "/portod";
constexpr const char *PORTO_HELPERS_CGROUP = "/portod-helpers";

constexpr const char *PORTO_GROUP_NAME = "porto";
constexpr const char *PORTO_SOCKET_PATH = "/run/portod.socket";
constexpr uint64_t PORTO_SOCKET_MODE = 0666;

constexpr int  REAP_EVT_FD = 128;
constexpr int  REAP_ACK_FD = 129;
constexpr int  PORTO_SK_FD = 130;

constexpr const char *PORTO_VERSION_FILE = "/run/portod.version";
constexpr const char *PORTO_BINARY_PATH = "/run/portod";

constexpr const char *PORTO_MASTER_PIDFILE = "/run/portoloop.pid";
constexpr const char *PORTO_PIDFILE = "/run/portod.pid";

constexpr const char *PORTOD_STAT_FILE = "/run/portod.stat";

constexpr const char *PORTOD_MASTER_NAME = "portod-master";
constexpr const char *PORTOD_NAME = "portod";

constexpr const char *PORTO_LOG = "/var/log/portod.log";

constexpr const char *PORTO_CONTAINERS_KV = "/run/porto/kvs";
constexpr const char *PORTO_VOLUMES_KV = "/run/porto/pkvs";

constexpr const char *PORTO_WORKDIR = "/place/porto";
constexpr const char *PORTO_PLACE = "/place";

constexpr const char *PORTO_VOLUMES = "porto_volumes";
constexpr const char *PORTO_LAYERS = "porto_layers";
constexpr const char *PORTO_STORAGE = "porto_storage";
constexpr const char *PORTO_DOCKER_IMAGES = "porto_docker/images";
constexpr const char *PORTO_DOCKER_LAYERS = "porto_docker/layers";

constexpr const char *PORTO_CHROOT_VOLUMES = "porto";

constexpr const char *PORTO_HELPERS_PATH = "/usr/lib/porto";

constexpr const char *PORTO_CACHE_QUOTA_FILE_NAME = "porto.volume.cache_quota_file";

constexpr const char *PORTO_PORTOCTL_PATH = "/usr/sbin/portoctl";

constexpr uint64_t CONTAINER_NAME_MAX = 128;
constexpr uint64_t CONTAINER_PATH_MAX = 200;
constexpr uint64_t CONTAINER_PATH_MAX_FOR_SUPERUSER = 220;
constexpr uint64_t RUN_SUBDIR_LIMIT = 100u;
constexpr uint64_t PRIVATE_VALUE_MAX = 4096;
constexpr uint64_t CONTAINER_COMMAND_MAX = 128 * 1024;    // ARG_MAX from include/uapi/linux/limits.h

constexpr const char *PORTO_NAME_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@:.";

constexpr unsigned PORTO_LABEL_NAME_LEN_MAX = 128;
constexpr unsigned PORTO_LABEL_VALUE_LEN_MAX = 256;
constexpr const char *PORTO_LABEL_PREFIX_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr unsigned PORTO_LABEL_PREFIX_LEN_MIN = 2;
constexpr unsigned PORTO_LABEL_PREFIX_LEN_MAX = 16;
constexpr unsigned PORTO_LABEL_COUNT_MAX = 100;

extern void AckExitStatus(int pid);
