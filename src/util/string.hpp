#pragma once

#include <string>
#include <vector>
#include <map>

#include "util/error.hpp"

typedef std::map<std::string, uint64_t> TUintMap;
typedef std::map<std::string, std::string> TStringMap;

TError StringToUint64(const std::string &string, uint64_t &value);
TError StringToInt64(const std::string &str, int64_t &value);
TError StringToInt(const std::string &string, int &value);
TError StringToOct(const std::string &str, unsigned &value);
TError StringToDouble(const std::string &str, double &value);

TError StringToBool(const std::string &str, bool &value);
std::string BoolToString(bool value);

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens, size_t maxFields = -1);

std::string MergeEscapeStrings(const std::vector<std::string> &list, char sep);
void SplitEscapedString(const std::string &str, std::vector<std::string> &list, char sep);

std::string StringTrim(const std::string& s, const std::string &what = " \t\n");
bool StringOnlyDigits(const std::string &s);
std::string StringReplaceAll(const std::string &str, const std::string &from, const std::string &to);
bool StringStartsWith(const std::string &str, const std::string &prefix);
bool StringEndsWith(const std::string &str, const std::string &suffix);
bool StringMatch(const std::string &str, const std::string &pattern);

typedef std::vector<std::pair<uint64_t, std::string>> TFlagsNames;
std::string StringFormatFlags(uint64_t flags,
                              const TFlagsNames &names,
                              const std::string sep = ",");
TError StringParseFlags(const std::string &str, const TFlagsNames &names,
                        uint64_t &result, const char sep = ',');

std::string StringFormat(const char *format, ...)
                         __attribute__ ((format (printf, 1, 2)));

std::string StringFormatSize(uint64_t value);

TError StringToValue(const std::string &str, double &value, std::string &unit);
TError StringToSize(const std::string &str, uint64_t &size);
TError StringToCpuValue(const std::string &str, double &value);

TError UintMapToString(const TUintMap &map, std::string &value);
TError StringToUintMap(const std::string &value, TUintMap &result);

std::string StringMapToString(const TStringMap &map);
TError StringToStringMap(const std::string &value, TStringMap &result);

int CompareVersions(const std::string &a, const std::string &b);
