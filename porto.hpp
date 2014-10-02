#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>

#include "version.hpp"

const std::string ROOT_CONTAINER = "/";
const std::string PORTO_ROOT_CGROUP = "porto";
const int REAP_EVT_FD = 128;
const int REAP_ACK_FD = 129;

const uint32_t DEF_CLASS_PRIO = 50;
const uint32_t DEF_CLASS_RATE = -1;
const uint32_t DEF_CLASS_CEIL = -1;
const uint32_t DEF_CLASS_NET_PRIO = 3;

#define NO_COPY_CONSTRUCT(NAME) \
    NAME(const NAME &) = delete; \
    NAME &operator=(const NAME &) = delete

#include "config.hpp"

#endif
