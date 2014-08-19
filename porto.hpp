#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>

#include "version.hpp"

const std::string RPC_SOCK = "/run/portod.socket";
const std::string RPC_SOCK_GROUP = "porto";
const int RPC_SOCK_PERM = 0660;

const std::string PID_FILE = "/run/portod.pid";
const int PID_FILE_PERM = 0644;

const std::string LOG_FILE = "/var/log/portod";
const int LOG_FILE_PERM = 0644;

#endif
