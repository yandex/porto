#include <sstream>
#include <iomanip>
#include <cstdarg>
#include <cctype>

#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fnmatch.h>
}

TError StringToUint64(const std::string &str, uint64_t &value) {
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
    return TError::Success();
}

TError StringToInt64(const std::string &str, int64_t &value) {
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
    return TError::Success();
}

TError StringToInt(const std::string &str, int &value) {
    int64_t val;
    if (StringToInt64(str, val) || val < INT32_MIN || val > INT32_MAX)
        return TError(EError::InvalidValue, "Bad int value: " + str);
    value = val;
    return TError::Success();
}

TError StringToOct(const std::string &str, unsigned &value) {
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
    return TError::Success();
}

TError StringToBool(const std::string &str, bool &value) {
    if (str == "true")
        value = true;
    else if (str == "false")
        value = false;
    else
        return TError(EError::Unknown, "Bad boolean value: " + str);
    return TError::Success();
}

std::string BoolToString(bool value) {
    return value ? "true" : "false";
}

TError StringToValue(const std::string &str, double &value, std::string &unit) {
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
    unit = std::string(end, len);
    return TError::Success();
}

static char size_unit[] = {'B', 'K', 'M', 'G', 'T', 'P', 'E', 0};

TError StringToSize(const std::string &str, uint64_t &size) {
    std::string unit;
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
    return TError::Success();
}

std::string StringFormatSize(uint64_t value)
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
std::string StringFormatDuration(uint64_t msec) {
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

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens, size_t maxFields) {
    if (!maxFields)
        return TError(EError::Unknown, "SplitString: invalid argument");

    try {
        std::istringstream ss(s);
        std::string tok;

        while(std::getline(ss, tok, sep)) {
            if (!--maxFields) {
                std::string rem;
                std::getline(ss, rem);
                if (rem.length()) {
                    tok += sep;
                    tok += rem;
                }
            }

            tokens.push_back(tok);
        }
    } catch (...) {
        return TError(EError::Unknown, "SplitString: Can't split string");
    }

    return TError::Success();
}

void SplitEscapedString(const std::string &str, TMultiTuple &tuples,
                        char sep_inner, char sep_outer) {
    std::stringstream ss;

    tuples.clear();
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
}


void SplitEscapedString(const std::string &str, TTuple &tuple, char sep) {
    std::vector<std::vector<std::string>> tuples;

    SplitEscapedString(str, tuples, sep, 0);

    if (tuples.size())
        tuple = tuples.front();
    else
        tuple.clear();
}

std::string MergeEscapeStrings(const TMultiTuple &tuples, char sep_inner, char sep_outer) {
    auto ssp_inner = std::string(1, sep_inner);
    auto rep_inner = "\\" + ssp_inner;
    auto spp_outer = std::string(1, sep_outer);
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

                std::string escaped = StringReplaceAll(str, "\\", "\\\\");
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

std::string MergeEscapeStrings(const TTuple &tuple, char sep) {
    TMultiTuple tuples = { tuple };
    return MergeEscapeStrings(tuples, sep, 0);
}

std::string StringTrim(const std::string& s, const std::string &what) {
    std::size_t first = s.find_first_not_of(what);
    std::size_t last  = s.find_last_not_of(what);

    if (first == std::string::npos || last == std::string::npos)
        return "";

    return s.substr(first, last - first + 1);
}

bool StringOnlyDigits(const std::string &s) {
    return s.find_first_not_of("0123456789") == std::string::npos;
}

std::string StringReplaceAll(const std::string &str, const std::string &from, const std::string &to) {
    std::string copy(str);

    std::string::size_type n = 0;
    while ((n = copy.find(from, n)) != std::string::npos) {
        copy.replace(n, from.size(), to);
        n += to.size();
    }

    return copy;
}

bool StringStartsWith(const std::string &str, const std::string &prefix) {
    if (str.length() < prefix.length())
        return false;

    return !str.compare(0, prefix.length(), prefix);
}

bool StringEndsWith(const std::string &str, const std::string &sfx) {
    if (str.length() < sfx.length())
        return false;

    return !str.compare(str.length() - sfx.length(), sfx.length(), sfx);
}

bool StringMatch(const std::string &str, const std::string &pattern) {
    if (pattern == "***")
        return true;
    return fnmatch(pattern.c_str(), str.c_str(), FNM_PATHNAME) == 0;
}

std::string StringFormatFlags(uint64_t flags,
                              const TFlagsNames &names,
                              const std::string sep) {
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

TError StringParseFlags(const std::string &str, const TFlagsNames &names,
                        uint64_t &result, const char sep) {
    std::stringstream ss(str);
    std::string name;

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
    return TError::Success();
}

std::string StringFormat(const char *format, ...) {
    std::string result;
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

TError StringToCpuValue(const std::string &str, double &value) {
    double val;
    std::string unit;

    TError error = StringToValue(str, val, unit);
    if (error || val < 0)
        return TError(EError::InvalidValue, "Invalid cpu value " + str);

    if (unit == "")
        value = val / 100 * GetNumCores();
    else if (unit == "c")
        value = val;
    else
        return TError(EError::InvalidValue, "Invalid cpu unit " + str);

    if (value < 0)
        return TError(EError::InvalidValue, "negative cpu count");

    return TError::Success();
}

TError UintMapToString(const TUintMap &map, std::string &value) {
    std::stringstream str;

    for (auto kv : map) {
        if (str.str().length())
            str << "; ";
        str << kv.first << ": " << kv.second;
    }

    value = str.str();

    return TError::Success();
}

TError StringToUintMap(const std::string &value, TUintMap &result) {
    std::vector<std::string> lines;
    TError error;

    SplitEscapedString(value, lines, ';');
    for (auto &line : lines) {
        std::vector<std::string> nameval;

        SplitEscapedString(line, nameval, ':');
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid format");

        std::string key = StringTrim(nameval[0]);
        uint64_t val;

        error = StringToSize(nameval[1], val);
        if (error)
            return TError(EError::InvalidValue, "Invalid value " + nameval[1]);

        result[key] = val;
    }

    return TError::Success();
}

std::string StringMapToString(const TStringMap &map) {
    std::stringstream str;

    for (auto kv : map) {
        if (str.str().length())
            str << "; ";
        str << kv.first << ": " << kv.second;
    }

    return str.str();
}

TError StringToStringMap(const std::string &value, TStringMap &result) {
    std::vector<std::string> lines;
    TError error;

    SplitEscapedString(value, lines, ';');
    for (auto &line : lines) {
        std::vector<std::string> nameval;

        SplitEscapedString(line, nameval, ':');
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid format");

        std::string key = StringTrim(nameval[0]);
        std::string val = StringTrim(nameval[1]);
        result[key] = val;
    }

    return TError::Success();
}

int CompareVersions(const std::string &a, const std::string &b) {
    return strverscmp(a.c_str(), b.c_str());
}

/* first[-last], ... */
TError TBitMap::Parse(const std::string &text) {
    TMultiTuple tuple;
    TError error;
    int first, last;

    bits.clear();
    SplitEscapedString(text, tuple, '-', ',');
    for (auto t: tuple) {
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
        if (bits.size() <= last)
            bits.resize(last + 1, false);
        std::fill(bits.begin() + first, bits.begin() + last + 1, true);
    }

    return TError::Success();
}

std::string TBitMap::Format() const {
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

TError TBitMap::Load(const TPath &path) {
    std::string text;
    TError error = path.ReadAll(text, 4096);
    if (error)
        return error;
    return Parse(text);
}

TError TBitMap::Save(const TPath &path) const {
    return path.WriteAll(Format());
}
