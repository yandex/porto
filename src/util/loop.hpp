#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

TError SetupLoopDevice(const TPath &imagePath, int &loopNr);
TError PutLoopDev(const int loopNr);
