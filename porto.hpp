#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>

#include "version.hpp"

const std::string ROOT_CONTAINER = "/";
const std::string PORTO_ROOT_CGROUP = "porto";
const int REAP_EVT_FD = 128;
const int REAP_ACK_FD = 129;

const ssize_t CONTAINER_MAX_LOG_SIZE = 10 * 1024 * 1024;
#define CONTAINER_TMP_DIR "/place/porto"
const size_t CONTAINER_AGING_TIME_MS = 60 * 60 * 24 * 7 * 1000;
const size_t RESPAWN_DELAY_MS = 1000;
const size_t STDOUT_READ_BYTES = 8 * 1024 * 1024;

const size_t PORTOD_MAX_CLIENTS = 16;
const size_t PORTOD_POLL_TIMEOUT_MS = 10 * 1000;
const size_t HEARTBEAT_DELAY_MS = 5 * 1000;
const size_t WATCHDOG_MAX_FAILS = 5;
const size_t WATCHDOG_DELAY_MS = 5 * 1000;
const unsigned int LOOP_WAIT_TIMEOUT_S = 10;
const unsigned int READ_WAIT_TIMEOUT_S = 5;
const size_t CGROUP_REMOVE_TIMEOUT_S = 1;
const size_t FREEZER_WAIT_TIMEOUT_S = 1;
const size_t MEMORY_GUARANTEE_RESERVE = 2 * 1024 * 1024 * 1024UL;

const uint32_t DEF_CLASS_PRIO = 50;
const uint32_t DEF_CLASS_RATE = -1;
const uint32_t DEF_CLASS_CEIL = -1;
const uint32_t DEF_CLASS_NET_PRIO = 3;

#define NO_COPY_CONSTRUCT(NAME) \
    NAME(const NAME &) = delete; \
    NAME &operator=(const NAME &) = delete

#include "config.hpp"

#endif
