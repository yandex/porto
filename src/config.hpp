#pragma once

#include <string>

#include "common.hpp"

#undef PROTOBUF_DEPRECATED
#define PROTOBUF_DEPRECATED __attribute__((deprecated))
#include "config.pb.h"
#undef PROTOBUF_DEPRECATED

extern cfg::TConfig &config();
void ReadConfigs(bool silent = false);
