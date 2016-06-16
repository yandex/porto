#pragma once

#include <string>
#include <vector>
#include <map>

#include "util/error.hpp"

typedef std::map<std::string, uint64_t> TUintMap;

std::string CommaSeparatedList(const std::vector<std::string> &list, const std::string &sep = ",");

TError StringsToIntegers(const std::vector<std::string> &strings,
                         std::vector<int> &integers);
TError StringToUint64(const std::string &string, uint64_t &value);
TError StringToInt64(const std::string &str, int64_t &value);
TError StringToInt(const std::string &string, int &value);
TError StringToOct(const std::string &str, unsigned &value);
TError StringToDouble(const std::string &str, double &value);

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens, size_t maxFields = -1);
TError SplitEscapedString(const std::string &s, const char sep, std::vector<std::string> &tokens);
std::string MergeEscapeStrings(std::vector<std::string> &strings, std::string sep, std::string rep);
std::string StringTrim(const std::string& s, const std::string &what = " \t\n");
bool StringOnlyDigits(const std::string &s);
std::string StringReplaceAll(const std::string &str, const std::string &from, const std::string &to);
bool StringStartsWith(const std::string &str, const std::string &prefix);

typedef std::vector<std::pair<uint64_t, std::string>> TFlagsNames;
std::string StringFormatFlags(uint64_t flags,
                              const TFlagsNames &names,
                              const std::string sep = ",");

std::string StringFormat(const char *format, ...)
                         __attribute__ ((format (printf, 1, 2)));

std::string StringFormatSize(uint64_t value);

TError StringToValue(const std::string &str, double &value, std::string &unit);
TError StringToSize(const std::string &str, uint64_t &size);
TError StringToStrList(const std::string &str, std::vector<std::string> &value);
TError StrListToString(const std::vector<std::string> lines, std::string &value);
