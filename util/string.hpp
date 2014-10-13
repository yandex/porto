#ifndef __STRINGUTIL_HPP__
#define __STRINGUTIL_HPP__

#include <cstdint>
#include <string>
#include <vector>
#include <set>

#include "error.hpp"

std::string CommaSeparatedList(const std::vector<std::string> &list);
std::string CommaSeparatedList(const std::set<std::string> &list);

TError StringsToIntegers(const std::vector<std::string> &strings,
                         std::vector<int> &integers);
TError StringToUint32(const std::string &str, uint32_t &value);
TError StringToUint64(const std::string &string, uint64_t &value);
TError StringToInt64(const std::string &str, int64_t &value);
TError StringToInt(const std::string &string, int &value);
TError StringWithUnitToUint64(const std::string &str, uint64_t &value);

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens, size_t maxFields = -1);
std::string StringTrim(const std::string& s);

std::string ReplaceMultiple(const std::string &str, const char rc);

#endif
