#ifndef __DATA_HPP__
#define __DATA_HPP__

#include "value.hpp"

// Data is not shown in the data list
const unsigned int HIDDEN_DATA = (1 << 0);

extern TValueSpec dataSpec;
TError RegisterData();

#endif
