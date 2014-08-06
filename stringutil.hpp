#ifndef __STRINGUTIL_HPP__
#define __STRINGUTIL_HPP__

#include "error.hpp"

#include <string>
#include <vector>
#include <set>

std::string CommaSeparatedList(const std::vector<std::string> &list);
std::string CommaSeparatedList(const std::set<std::string> &list);

TError StringsToIntegers(std::vector<std::string> &strings,
                         std::vector<int> &integers);

#endif
