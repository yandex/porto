#ifndef __STAT_HPP__
#define __STAT_HPP__

#include <string>

#include "porto.hpp"

void StatInc(const std::string &name);
void StatReset(const std::string &name);
int StatGet(const std::string &name);

#endif
