#ifndef __DATA_HPP__
#define __DATA_HPP__

#include "value.hpp"

const std::string D_OOM_KILLED = "oom_killed";

extern TValueSpec dataSpec;
TError RegisterData();

#endif
