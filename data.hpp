#pragma once

#include "value.hpp"

const std::string D_OOM_KILLED = "oom_killed";
const std::string D_ROOT_PID = "root_pid";
const std::string D_EXIT_STATUS = "exit_status";
const std::string D_START_ERRNO = "start_errno";
const std::string D_RESPAWN_COUNT = "respawn_count";
const std::string D_STATE = "state";
const std::string D_MAX_RSS = "max_rss";

extern TValueSet dataSet;
TError RegisterData();
