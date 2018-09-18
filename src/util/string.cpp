#include <sstream>
#include <iomanip>
#include <cstdarg>
#include <cctype>

#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fnmatch.h>
}

TError StringToUint64(const TString &str, uint64_t &value) {
    const char *ptr = str.c_str();
    char *end;

    errno = 0;
    value = strtoull(ptr, &end, 10);
    if (errno || end == ptr)
        return TError(EError::InvalidValue, errno, "Bad uint64 value: " + str);
    while (isspace(*end))
        end++;
    if (*end)
        return TError(EError::InvalidValue, "Bad uint64 value: " + str);
    return OK;
}

TError StringToInt64(const TString &str, int64_t &value) {
    const char *ptr = str.c_str();
    char *end;

    errno = 0;
    value = strtoll(ptr, &end, 10);
    if (errno || end == ptr)
        return TError(EError::InvalidValue, errno, "Bad int64 value: " + str);
    while (isspace(*end))
        end++;
    if (*end)
        return TError(EError::InvalidValue, "Bad int64 value: " + str);
    return OK;
}

TError StringToInt(const TString &str, int &value) {
    int64_t val;
    if (StringToInt64(str, val) || val < INT32_MIN || val > INT32_MAX)
        return TError(EError::InvalidValue, "Bad int value: " + str);
    value = val;
    return OK;
}

TError StringToOct(const TString &str, unsigned &value) {
    const char *ptr = str.c_str();
    char *end;

    errno = 0;
    uint64_t val = strtoull(ptr, &end, 8);
    if (errno || end == ptr || val > UINT32_MAX)
        return TError(EError::InvalidValue, errno, "Bad oct value: " + str);
    while (isspace(*end))
        end++;
    if (*end)
        return TError(EError::InvalidValue, "Bad oct value: " + str);
    value = val;
    return OK;
}

TError StringToBool(const TString &str, bool &value) {
    if (str == "true")
        value = true;
    else if (str == "false")
        value = false;
    else
        return TError("Bad boolean value: " + str);
    return OK;
}

TString BoolToString(bool value) {
    return value ? "true" : "false";
}

TError StringToValue(const TString &str, double &value, TString &unit) {
    const char *ptr = str.c_str();
    char *end;

    errno = 0;
    value = strtod(ptr, &end);
    if (errno || end == ptr)
        return TError(EError::InvalidValue, errno, "Bad value: " + str);

    while (isspace(*end))
        end++;
    size_t len = strlen(end);
    while (len && isspace(end[len-1]))
        len--;
    unit = TString(end, len);
    return OK;
}

static char size_unit[] = {'B', 'K', 'M', 'G', 'T', 'P', 'E', 0};

TError StringToSize(const TString &str, uint64_t &size) {
    TString unit;
    uint64_t mult = 1;
    double value;
    TError error;

    error = StringToValue(str, value, unit);
    if (error)
        return error;

    if (value < 0)
        return TError(EError::InvalidValue, "Negative: " + str);

    if (!unit[0])
        goto ok;

    for (int i = 0; size_unit[i]; i++) {
        if (unit[0] == size_unit[i] ||
            unit[0] == tolower(size_unit[i])) {

            mult = 1ull << (10 * i);

            /* allow K Kb kB KiB */
            switch (unit[1]) {
                case 'b': /* FIXME turn into bits? */
                case 'B':
                    if (i && unit[2] == '\0')
                        goto ok;
                    break;
                case 'i':
                    if (!i || unit[2] != 'B')
                        break;
                case '\0':
                    goto ok;
            }

            break;
        }
    }

    return TError(EError::InvalidValue, "Bad value unit: " + unit);

ok:
    if (value * mult > UINT64_MAX)
        return TError(EError::InvalidValue, "Too big: " + str);

    size = value * mult;
    return OK;
}

TString StringFormatSize(uint64_t value)
{
    int i = 0;

    while (value >= (1ull<<(10*(i+1))) && size_unit[i+1])
        i++;

    uint64_t div = 1ull << (10 * i);

    if (value % div == 0)
        return StringFormat("%llu%c", (unsigned long long)value / div, size_unit[i]);

    return StringFormat("%.1f%c", (double)value / div, size_unit[i]);
}

/* 10.123s or H:MM:SS or Dd H:MM */
TString StringFormatDuration(uint64_t msec) {
    if (msec < 60000)
        return StringFormat("%gs", msec / 1000.);
    int seconds = msec / 1000 % 60;
    int minutes = msec / (60*1000) % 60;
    int hours = msec / (60*60*1000) % 24;
    int days = msec / (24*60*60*1000);
    if (!days)
        return StringFormat("%d:%02d:%02d", hours, minutes, seconds);
    return StringFormat("%dd %2d:%02d", days, hours, minutes);
}

TError StringToNsec(const TString &str, uint64_t &nsec) {
    TString unit;
    uint64_t mult = 1;
    double value;
    TError error;

    error = StringToValue(str, value, unit);
    if (error)
        return error;

    if (value < 0)
        return TError(EError::InvalidValue, "Negative: " + str);

    if (!unit[0])
        goto ok;

    if (unit == "s" || unit == "sec")
        mult = 1000000000;
    else if (unit == "ms" || unit == "msec")
        mult = 1000000;
    else if (unit == "us" || unit == "usec")
        mult = 1000;
    else if (unit == "ns" || unit == "nsec")
        mult = 1;
    else if (unit == "ps" || unit == "psec")
        mult = 0.001;
    else if (unit == "fs" || unit == "fsec")
        mult = 0.000001;
    else
        return TError(EError::InvalidValue, "Unknown unit: " + unit);

ok:
    if (value * mult > UINT64_MAX)
        return TError(EError::InvalidValue, "Too big: " + str);

    nsec = value * mult;
    return OK;
}

TTuple SplitString(const TString &str, const char sep, int max) {
    std::vector<TString> tokens;
    std::istringstream ss(str);
    TString tok;

    while(std::getline(ss, tok, sep)) {
        if (max && !--max) {
            TString rem;
            std::getline(ss, rem);
            if (rem.length()) {
                tok += sep;
                tok += rem;
            }
        }
        tokens.push_back(tok);
    }

    return tokens;
}

TMultiTuple SplitEscapedString(const TString &str, char sep_inner, char sep_outer) {
    std::stringstream ss;
    TMultiTuple tuples;

    tuples.push_back({});

    auto i = str.begin();

    while (true) {
        if (*i == sep_inner || (sep_outer && *i == sep_outer) || i == str.end()) {
            auto s = StringTrim(ss.str());
            if (s.size())
                tuples.back().push_back(s);

            ss.str("");

            if (i == str.end())
                break;

            if ((sep_outer && *i == sep_outer) && tuples.back().size())
                tuples.push_back({});

        } else if (*i == '\\' && ((i + 1) != str.end()) &&
                   ((*(i + 1) == '\\') || *(i + 1) == sep_inner ||
                   (sep_outer && *(i + 1) == sep_outer))) {

            ss << *(i + 1);
            i++;
        } else {
            /* Backslash without escape goes into string */
            ss << *i;
        }

        ++i;
    }

    if (!tuples.back().size())
        tuples.pop_back();

    return tuples;
}

TTuple SplitEscapedString(const TString &str, char sep) {
    auto tuples = SplitEscapedString(str, sep, 0);
    if (tuples.size())
        return tuples.front();
    return TTuple();
}

TString MergeEscapeStrings(const TMultiTuple &tuples, char sep_inner, char sep_outer) {
    auto ssp_inner = TString(1, sep_inner);
    auto rep_inner = "\\" + ssp_inner;
    auto spp_outer = TString(1, sep_outer);
    auto rep_outer = "\\" + spp_outer;
    std::stringstream ss;
    bool first_outer = true;

    for (auto &tuple : tuples) {
        if (tuple.size()) {
            bool first_inner = true;

            if (!first_outer)
                ss << spp_outer;

            first_outer = false;

            for (auto &str : tuple) {
                if (!first_inner)
                    ss << ssp_inner;

                first_inner = false;

                TString escaped = StringReplaceAll(str, "\\", "\\\\");
                escaped = StringReplaceAll(escaped, ssp_inner, rep_inner);

                if (sep_outer)
                    escaped = StringReplaceAll(escaped, spp_outer, rep_outer);

                ss << escaped;
            }
        }

        if (!sep_outer)
            break;
    }

    return ss.str();
}

TString MergeEscapeStrings(const TTuple &tuple, char sep) {
    TMultiTuple tuples = { tuple };
    return MergeEscapeStrings(tuples, sep, 0);
}

TString StringTrim(const TString& s, const TString &what) {
    std::size_t first = s.find_first_not_of(what);
    std::size_t last  = s.find_last_not_of(what);

    if (first == TString::npos || last == TString::npos)
        return "";

    return s.substr(first, last - first + 1);
}

bool StringOnlyDigits(const TString &s) {
    return s.find_first_not_of("0123456789") == TString::npos;
}

TString StringReplaceAll(const TString &str, const TString &from, const TString &to) {
    TString copy(str);

    TString::size_type n = 0;
    while ((n = copy.find(from, n)) != TString::npos) {
        copy.replace(n, from.size(), to);
        n += to.size();
    }

    return copy;
}

bool StringStartsWith(const TString &str, const TString &prefix) {
    if (str.length() < prefix.length())
        return false;

    return !str.compare(0, prefix.length(), prefix);
}

bool StringEndsWith(const TString &str, const TString &sfx) {
    if (str.length() < sfx.length())
        return false;

    return !str.compare(str.length() - sfx.length(), sfx.length(), sfx);
}

bool StringMatch(const TString &str, const TString &pattern) {
    if (pattern == "***")
        return true;
    return fnmatch(pattern.c_str(), str.c_str(), FNM_PATHNAME) == 0;
}

TString StringFormatFlags(uint64_t flags,
                              const TFlagsNames &names,
                              const TString sep) {
    std::stringstream result;
    bool first = true;

    for (auto &n : names) {
        if (n.first & flags) {
            if (first)
                first = false;
            else
                result << sep;
            result << n.second;
            flags &= ~n.first;
        }
    }

    if (flags) {
        if (!first)
            result << sep;
        result << std::hex << flags;
    }

    return result.str();
}

TError StringParseFlags(const TString &str, const TFlagsNames &names,
                        uint64_t &result, const char sep) {
    std::stringstream ss(str);
    TString name;

    result = 0;
    while (std::getline(ss, name, sep)) {
        bool found = false;
        name = StringTrim(name);
        if (name.empty())
            continue;
        for (auto &n: names) {
            if (n.second == name) {
                result |= n.first;
                found = true;
                break;
            }
        }
        if (!found)
            return TError(EError::InvalidValue, "Unknown flag \"" + name + "\"");
    }
    return OK;
}

TString StringFormat(const char *format, ...) {
    TString result;
    int length;
    va_list ap;

    va_start(ap, format);
    length = vsnprintf(nullptr, 0, format, ap);
    va_end(ap);

    result.resize(length);

    va_start(ap, format);
    vsnprintf(&result[0], length + 1, format, ap);
    va_end(ap);

    return result;
}

TError StringToCpuPower(const TString &str, uint64_t &power) {
    double val;
    TString unit;

    TError error = StringToValue(str, val, unit);
    if (error || val < 0)
        return TError(EError::InvalidValue, "Invalid cpu power value " + str);

    if (unit == "")
        power = val * NSEC_PER_SEC / 100 * GetNumCores();
    else if (unit == "c")
        power = val * NSEC_PER_SEC;
    else if (unit == "ns")
        power = val;
    else
        return TError(EError::InvalidValue, "Invalid cpu power unit " + str);

    return OK;
}

TString CpuPowerToString(uint64_t nsec) {
    return fmt::format("{:g}c", (double)nsec / NSEC_PER_SEC);
}

TError UintMapToString(const TUintMap &map, TString &value) {
    std::stringstream str;

    for (auto kv : map) {
        if (str.str().length())
            str << "; ";
        str << kv.first << ": " << kv.second;
    }

    value = str.str();

    return OK;
}

TError StringToUintMap(const TString &value, TUintMap &result) {
    TError error;

    for (auto &line : SplitEscapedString(value, ';')) {
        auto nameval = SplitEscapedString(line, ':');
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid format");

        TString key = StringTrim(nameval[0]);
        uint64_t val;

        error = StringToSize(nameval[1], val);
        if (error)
            return TError(EError::InvalidValue, "Invalid value " + nameval[1]);

        result[key] = val;
    }

    return OK;
}

TString StringMapToString(const TStringMap &map) {
    std::stringstream str;

    for (auto kv : map) {
        if (str.str().length())
            str << "; ";
        str << kv.first << ": " << kv.second;
    }

    return str.str();
}

TError StringToStringMap(const TString &value, TStringMap &result) {
    TError error;

    for (auto &line : SplitEscapedString(value, ';')) {
        auto nameval = SplitEscapedString(line, ':');
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid format");

        TString key = StringTrim(nameval[0]);
        TString val = StringTrim(nameval[1]);
        result[key] = val;
    }

    return OK;
}

int CompareVersions(const TString &a, const TString &b) {
    return strverscmp(a.c_str(), b.c_str());
}

/* first[-last], ... */
TError TPortoBitMap::Parse(const TString &text) {
    TError error;
    int first, last;

    bits.clear();
    for (auto &t: SplitEscapedString(text, '-', ',')) {
        if (t.size() == 0)
            continue;
        if (t.size() > 2)
            return TError(EError::InvalidValue, "wrong bitmap format");
        error = StringToInt(t[0], first);
        if (error || first < 0 || first > 65535)
            return TError(EError::InvalidValue, "wrong bitmap format");
        if (t.size() == 2) {
            error = StringToInt(t[1], last);
            if (error || last < first || last > 65535)
                return TError(EError::InvalidValue, "wrong bitmap format");
        } else
            last = first;
        if ((int)bits.size() <= last)
            bits.resize(last + 1, false);
        std::fill(bits.begin() + first, bits.begin() + last + 1, true);
    }

    return OK;
}

TString TPortoBitMap::Format() const {
    bool prev = false, curr, sep = false, range = false;
    std::stringstream ss;

    for (unsigned i = 0; i <= bits.size(); i++, prev = curr) {
        curr = Get(i);
        if (prev == curr)
            range = true;
        else if (curr) {
            if (sep)
                ss << ",";
            else
                sep = true;
            ss << i;
            range = false;
        } else if (range)
            ss << "-" << i - 1;
    }

    return ss.str();
}

TError TPortoBitMap::Read(const TPath &path) {
    TString text;
    TError error = path.ReadAll(text, 4096);
    if (error)
        return error;
    return Parse(text);
}

TError TPortoBitMap::Write(const TPath &path) const {
    return path.WriteAll(Format());
}
