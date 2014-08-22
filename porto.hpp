#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>

#include "version.hpp"

const bool LOG_VEBOSE = true;

const std::string ROOT_CONTAINER = "/";
const std::string ROOT_CGROUP = "porto";
const std::string CONTAINER_BASEDIR = "/db/porto/";
const ssize_t CONTAINER_MAX_LOG_SIZE = 10 * 1024 * 1024;
const std::string KVALUE_ROOT = "/tmp/porto";
const std::string KVALUE_SIZE = "size=32m";
const size_t PORTOD_MAX_CLIENTS = 16;
const size_t PORTOD_POLL_TIMEOUT_MS = 1000;
const size_t CGROUP_REMOVE_TIMEOUT_S = 1;
const size_t FREEZER_WAIT_TIMEOUT_S = 1;
const size_t STDOUT_READ_BYTES = 8 * 1024 * 1024;

const std::string RPC_SOCK = "/run/portod.socket";
const std::string RPC_SOCK_GROUP = "porto";
const int RPC_SOCK_PERM = 0660;

const std::string PID_FILE = "/run/portod.pid";
const int PID_FILE_PERM = 0644;

const std::string LOG_FILE = "/var/log/portod";
const int LOG_FILE_PERM = 0644;

#endif
