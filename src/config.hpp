#pragma once

#include <string>

#include "common.hpp"

#include "config.pb.h"

extern cfg::TConfig &config();
void ReadConfigs(bool silent = false);
