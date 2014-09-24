#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>

#include "version.hpp"

const bool LOG_VEBOSE = true;
const bool DEBUG_NETLINK = false;

const std::string ROOT_CONTAINER = "/";
const std::string INIT_CONTAINER = "system";

const std::string PORTO_ROOT_CGROUP = "porto";
const std::string CONTAINER_BASEDIR = "/db/porto/";
const ssize_t CONTAINER_MAX_LOG_SIZE = 10 * 1024 * 1024;
const std::string KVALUE_ROOT = "/run/porto/kvs";
const std::string KVALUE_SIZE = "size=32m";
const size_t PORTOD_MAX_CLIENTS = 16;
const size_t PORTOD_POLL_TIMEOUT_MS = 1000;
const size_t CGROUP_REMOVE_TIMEOUT_S = 1;
const size_t FREEZER_WAIT_TIMEOUT_S = 1;
const size_t STDOUT_READ_BYTES = 8 * 1024 * 1024;

const size_t CONTAINER_AGING_TIME_MS = 60 * 60 * 24 * 7 * 1000;
const size_t RESPAWN_DELAY_MS = 1000;
const size_t HEARTBEAT_DELAY_MS = 1000;
const size_t WATCHDOG_MAX_FAILS = 5;

const size_t MEMORY_GUARANTEE_RESERVE = 2 * 1024 * 1024 * 1024UL;

const unsigned int KVS_PERM = 0600;

const std::string RPC_SOCK = "/run/portod.socket";
const std::string RPC_SOCK_GROUP = "porto";
const unsigned int RPC_SOCK_PERM = 0660;

const std::string PID_FILE = "/run/portod.pid";
const unsigned int PID_FILE_PERM = 0644;

const std::string LOOP_PID_FILE = "/run/portoloop.pid";
const unsigned int LOOP_PID_FILE_PERM = 0644;

const std::string LOG_FILE = "/var/log/portod.log";
const unsigned int LOG_FILE_PERM = 0644;

const std::string LOOP_LOG_FILE = "/var/log/portoloop.log";
const unsigned int LOOP_LOG_FILE_PERM = 0644;

const unsigned int LOOP_WAIT_TIMEOUT_S = 10;

const int REAP_EVT_FD = 128;
const int REAP_ACK_FD = 129;

const uint32_t DEF_CLASS_PRIO = 50;
const uint32_t DEF_CLASS_RATE = UINT32_MAX;
const uint32_t DEF_CLASS_CEIL = UINT32_MAX;
const uint32_t DEF_CLASS_NET_PRIO = 3;

#define NO_COPY_CONSTRUCT(NAME) \
    NAME(const NAME &) = delete; \
    NAME &operator=(const NAME &) = delete

#endif
