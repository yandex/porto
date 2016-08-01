#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

TError SetupLoopDevice(TPath image, int &dev);
TError PutLoopDev(const int nr);
