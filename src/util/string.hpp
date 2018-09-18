#pragma once

#include <string>
#include <vector>
#include <map>

#include "util/error.hpp"

typedef std::map<TString, uint64_t> TUintMap;
typedef std::map<TString, TString> TStringMap;
typedef std::vector<TString> TTuple;
typedef std::vector<TTuple> TMultiTuple;

TError StringToUint64(const TString &string, uint64_t &value);
TError StringToInt64(const TString &str, int64_t &value);
TError StringToInt(const TString &string, int &value);
TError StringToOct(const TString &str, unsigned &value);

TError StringToBool(const TString &str, bool &value);
TString BoolToString(bool value);

TTuple SplitString(const TString &str, const char sep, int max = 0);

TMultiTuple SplitEscapedString(const TString &str, char sep_inner, char sep_outer);
TTuple SplitEscapedString(const TString &str, char sep);

TString MergeEscapeStrings(const TMultiTuple &tuples,
                               char sep_inner, char sep_outer);
TString MergeEscapeStrings(const TTuple &tuple, char sep);

TString StringTrim(const TString& s, const TString &what = " \t\n");
bool StringOnlyDigits(const TString &s);
TString StringReplaceAll(const TString &str, const TString &from, const TString &to);
bool StringStartsWith(const TString &str, const TString &prefix);
bool StringEndsWith(const TString &str, const TString &suffix);
bool StringMatch(const TString &str, const TString &pattern);

typedef std::vector<std::pair<uint64_t, TString>> TFlagsNames;
TString StringFormatFlags(uint64_t flags,
                              const TFlagsNames &names,
                              const TString sep = ",");
TError StringParseFlags(const TString &str, const TFlagsNames &names,
                        uint64_t &result, const char sep = ',');

TString StringFormat(const char *format, ...)
                         __attribute__ ((format (printf, 1, 2)));

TString StringFormatSize(uint64_t value);
TString StringFormatDuration(uint64_t msec);

TError StringToValue(const TString &str, double &value, TString &unit);
TError StringToSize(const TString &str, uint64_t &size);
TError StringToNsec(const TString &str, uint64_t &nsec);

TError StringToCpuPower(const TString &str, uint64_t &nsec);
TString CpuPowerToString(uint64_t nsec);

TError UintMapToString(const TUintMap &map, TString &value);
TError StringToUintMap(const TString &value, TUintMap &result);

TString StringMapToString(const TStringMap &map);
TError StringToStringMap(const TString &value, TStringMap &result);

int CompareVersions(const TString &a, const TString &b);

class TPath;

class TPortoBitMap {
private:
    std::vector<bool> bits;
public:
    TPortoBitMap() {}
    ~TPortoBitMap() {}
    TError Parse(const TString &text);
    TString Format() const;
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

    bool Get(unsigned index) const {
        return index < Size() && bits[index];
    }

    void Set(unsigned index, bool val = true) {
        if (index >= Size())
            bits.resize(index + 1, false);
        bits[index] = val;
    }

    void Set(const TPortoBitMap &map, bool val = true) {
        for (unsigned i = 0; i < map.Size(); i++)
            if (map.Get(i))
                Set(i, val);
    }

    void Clear() {
        bits.clear();
    }

    bool IsSubsetOf(const TPortoBitMap &map) const {
        for (unsigned i = 0; i < Size(); i++)
            if (Get(i) && !map.Get(i))
                return false;
        return true;
    }

    bool IsEqual(const TPortoBitMap &map) const {
        for (unsigned i = 0; i < std::max(Size(), map.Size()); i++)
            if (Get(i) != map.Get(i))
                return false;
        return true;
    }
};
