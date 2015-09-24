#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <map>

#include "error.hpp"

std::string CommaSeparatedList(const std::vector<std::string> &list, const std::string &sep = ",");
std::string CommaSeparatedList(const std::set<std::string> &list);

TError StringsToIntegers(const std::vector<std::string> &strings,
                         std::vector<int> &integers);
TError StringToUint16(const std::string &str, uint16_t &value);
TError StringToUint32(const std::string &str, uint32_t &value);
TError StringToUint64(const std::string &string, uint64_t &value);
TError StringToInt64(const std::string &str, int64_t &value);
TError StringToInt(const std::string &string, int &value);
TError StringToOct(const std::string &str, unsigned &value);
TError StringToDouble(const std::string &str, double &value);
TError StringWithUnitToUint64(const std::string &str, uint64_t &value);
std::string StringWithUnit(uint64_t value, int precision = 1);

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens, size_t maxFields = -1);
TError SplitEscapedString(const std::string &s, const char sep, std::vector<std::string> &tokens);
std::string MergeEscapeStrings(std::vector<std::string> &strings, std::string sep, std::string rep);
std::string StringTrim(const std::string& s, const std::string &what = " \t\n");
bool StringOnlyDigits(const std::string &s);
std::string StringReplaceAll(const std::string &str, const std::string &from, const std::string &to);
bool StringStartsWith(const std::string &str, const std::string &prefix);
bool StringEndsWith(const std::string &str, const std::string &prefix);
std::string MapToStr(const std::map<std::string, uint64_t> &m);
