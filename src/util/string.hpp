#pragma once

#include <string>
#include <vector>
#include <map>

#include "util/error.hpp"

#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

typedef std::map<std::string, uint64_t> TUintMap;
typedef std::map<std::string, std::string> TStringMap;
typedef std::vector<std::string> TTuple;
typedef std::vector<TTuple> TMultiTuple;

TError StringToUint64(const std::string &string, uint64_t &value);
TError StringToInt64(const std::string &str, int64_t &value);
TError StringToInt(const std::string &string, int &value);
TError StringToOct(const std::string &str, unsigned &value);

TError StringToBool(const std::string &str, bool &value);
std::string BoolToString(bool value);

TTuple SplitString(const std::string &str, const char sep, int max = 0);

TMultiTuple SplitEscapedString(const std::string &str, char sep_inner, char sep_outer);
TTuple SplitEscapedString(const std::string &str, char sep);

std::string MergeEscapeStrings(const TMultiTuple &tuples,
                               char sep_inner, char sep_outer);
std::string MergeEscapeStrings(const TTuple &tuple, char sep);

std::string StringTrim(const std::string& s, const std::string &what = " \t\n");
bool StringOnlyDigits(const std::string &s);
std::string StringReplaceAll(const std::string &str, const std::string &from, const std::string &to);
bool StringStartsWith(const std::string &str, const std::string &prefix);
bool StringEndsWith(const std::string &str, const std::string &suffix);
bool StringMatch(const std::string &str, const std::string &pattern, bool strict = false);
bool StringSubpath(const std::string &path, const std::string &subpath);

typedef std::vector<std::pair<uint64_t, std::string>> TFlagsNames;
std::string StringFormatFlags(uint64_t flags,
                              const TFlagsNames &names,
                              const std::string sep = ",");
TError StringParseFlags(const std::string &str, const TFlagsNames &names,
                        uint64_t &result, const char sep = ',');

std::string StringFormat(const char *format, ...)
                         __attribute__ ((format (printf, 1, 2)));

std::string StringFormatSize(uint64_t value);
std::string StringFormatDuration(uint64_t msec);

TError StringToValue(const std::string &str, double &value, std::string &unit);
TError StringToSize(const std::string &str, uint64_t &size);
TError StringToNsec(const std::string &str, uint64_t &nsec);

TError StringToCpuPower(const std::string &str, uint64_t &nsec);
std::string CpuPowerToString(uint64_t nsec);

TError UintMapToString(const TUintMap &map, std::string &value);
TError StringToUintMap(const std::string &value, TUintMap &result);

std::string StringMapToString(const TStringMap &map);
TError StringToStringMap(const std::string &value, TStringMap &result);

int CompareVersions(const std::string &a, const std::string &b);

class TPath;

class TBitMap {
private:
    std::vector<bool> bits;
public:
    TBitMap() {}
    ~TBitMap() {}
    TError Parse(const std::string &text);
    std::string Format() const;
    TError Read(const TPath &path);
    TError Write(const TPath &path) const;

    unsigned Size() const {
        return bits.size();
    }

    unsigned Weight() const {
        unsigned weight = 0;
        for (auto bit: bits)
            weight += bit;
        return weight;
    }

    int FirstBitIndex(unsigned from = 0) {
        for (unsigned i = from; i < bits.size(); i++) {
            if (bits[i])
                return i;
        }

        for (unsigned i = 0; i < from; i++) {
            if (bits[i])
                return i;
        }

        return -1;
    }

    bool Get(unsigned index) const {
        return index < Size() && bits[index];
    }

    void Set(unsigned index, bool val = true) {
        if (index >= Size())
            bits.resize(index + 1, false);
        bits[index] = val;
    }

    void Set(const TBitMap &map, bool val = true) {
        for (unsigned i = 0; i < map.Size(); i++)
            if (map.Get(i))
                Set(i, val);
    }

    void Clear() {
        bits.clear();
    }

    bool IsSubsetOf(const TBitMap &map) const {
        for (unsigned i = 0; i < Size(); i++)
            if (Get(i) && !map.Get(i))
                return false;
        return true;
    }

    bool IsEqual(const TBitMap &map) const {
        for (unsigned i = 0; i < std::max(Size(), map.Size()); i++)
            if (Get(i) != map.Get(i))
                return false;
        return true;
    }
};
