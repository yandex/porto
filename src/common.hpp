#pragma once

#include "util/error.hpp"

#define __STDC_LIMIT_MACROS
#include <cstdint>
#undef __STDC_LIMIT_MACROS

#define noinline __attribute__((noinline))

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

constexpr auto ROOT_TC_MAJOR = 1;
constexpr auto ROOT_TC_MINOR = 0;
constexpr auto DEFAULT_TC_MINOR = 2;

/* FIXME add support for 64-bit rate and ceil */
const uint64_t NET_MAX_LIMIT = UINT32_MAX;
const uint64_t NET_MAX_GUARANTEE = UINT32_MAX;
constexpr auto NET_DEFAULT_PRIO = 3;

constexpr auto ROOT_CONTAINER_ID = 1;
constexpr auto PORTO_ROOT_CONTAINER_ID = 3;

constexpr auto ROOT_CONTAINER = "/";
constexpr auto DOT_CONTAINER = ".";
constexpr auto PORTO_ROOT_CONTAINER = "/porto";

constexpr auto PORTO_ROOT_CGROUP = "/porto";
constexpr auto PORTO_DAEMON_CGROUP = "/portod";

constexpr auto PORTO_GROUP_NAME = "porto";
constexpr auto PORTO_SOCKET_PATH = "/run/portod.socket";
constexpr auto PORTO_SOCKET_MODE = 0666;

constexpr auto PORTO_VERSION_FILE = "/run/portod.version";

constexpr auto CONTAINER_NAME_MAX = 128;
constexpr auto CONTAINER_PATH_MAX = 200;
constexpr auto CONTAINER_ID_MAX = 16384;

extern void AckExitStatus(int pid);
