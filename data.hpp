#ifndef __DATA_HPP__
#define __DATA_HPP__

#include "value.hpp"

const std::string D_OOM_KILLED = "oom_killed";
const std::string D_ROOT_PID = "root_pid";
const std::string D_EXIT_STATUS = "exit_status";
const std::string D_START_ERRNO = "start_errno";

extern TValueSet dataSet;
TError RegisterData();

#endif
