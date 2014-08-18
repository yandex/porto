#ifndef __STRINGUTIL_HPP__
#define __STRINGUTIL_HPP__

#include "error.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <set>

std::string CommaSeparatedList(const std::vector<std::string> &list);
std::string CommaSeparatedList(const std::set<std::string> &list);

TError StringsToIntegers(const std::vector<std::string> &strings,
                         std::vector<int> &integers);
TError StringToUint64(const std::string &string, uint64_t &value);
TError StringToInt(const std::string &string, int &value);

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens);

#endif
