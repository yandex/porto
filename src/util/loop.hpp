#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

TError SetupLoopDev(int &loopNr, const TPath &image, bool ro);
TError PutLoopDev(int loopNr);
