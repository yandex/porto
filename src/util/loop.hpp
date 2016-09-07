#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

TError SetupLoopDev(int &loopNr, const TPath &image, bool ro);
TError PutLoopDev(int loopNr);
TError ResizeLoopDev(int loopNr, const TPath &image, off_t current, off_t target);
