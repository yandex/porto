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

constexpr int ROOT_TC_MAJOR = 1;
constexpr int ROOT_TC_MINOR = 0;
constexpr int DEFAULT_TC_MINOR = 2;
constexpr int DEFAULT_TC_MAJOR = 2;
constexpr int CONTAINER_TC_MINOR = 0;

constexpr uint64_t CPU_POWER_PER_SEC = 1000000000ull;

constexpr uint64_t NET_DEFAULT_PRIO = 3;
constexpr uint64_t NET_MAX_RATE = 2000000000; /* 16Gbit */

constexpr uint64_t ROOT_CONTAINER_ID = 1;
constexpr uint64_t LEGACY_CONTAINER_ID = 3;

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
constexpr const char *PORTO_CT_GROUP_NAME = "porto-containers";
constexpr const char *USER_CT_SUFFIX = "-containers";
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

constexpr const char *PORTO_CHROOT_VOLUMES = "porto";

constexpr uint64_t CONTAINER_NAME_MAX = 128;
constexpr uint64_t CONTAINER_PATH_MAX = 200;
constexpr uint64_t CONTAINER_PATH_MAX_FOR_SUPERUSER = 220;
constexpr uint64_t CONTAINER_ID_MAX = 16384;
constexpr uint64_t CONTAINER_LEVEL_MAX = 16;
constexpr uint64_t RUN_SUBDIR_LIMIT = 100u;

constexpr const char *PORTO_NAME_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@:.";

constexpr const char *PORTO_WEAK_PREFIX = "_weak_";

extern void AckExitStatus(int pid);

extern std::string PreviousVersion;
