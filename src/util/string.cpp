#include <sstream>
#include <iomanip>
#include <cstdarg>
#include <cctype>

#include "util/string.hpp"

using std::string;
using std::vector;
using std::set;
using std::istringstream;
using std::stringstream;

string CommaSeparatedList(const vector<string> &list, const std::string &sep) {
    string ret;
    for (auto c = list.begin(); c != list.end(); ) {
        ret += *c;
        if (++c != list.end())
            ret += sep;
    }
    return ret;
}

string CommaSeparatedList(const set<string> &list) {
    string ret;
    for (auto c = list.begin(); c != list.end(); ) {
        ret += *c;
        if (++c != list.end())
            ret += ",";
    }
    return ret;
}

TError StringsToIntegers(const std::vector<std::string> &strings,
                         std::vector<int> &integers) {
    for (auto l : strings) {
        try {
            integers.push_back(stoi(l));
        } catch (...) {
            return TError(EError::Unknown, string(__func__) + ": Bad integer value " + l);
        }
    }

    return TError::Success();
}

TError StringToUint64(const std::string &str, uint64_t &value) {
    try {
        value = stoull(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value " + str);
    }

    return TError::Success();
}

TError StringToInt64(const std::string &str, int64_t &value) {
    try {
        value = stoll(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value " + str);
    }

    return TError::Success();
}

TError StringToInt(const std::string &str, int &value) {
    try {
        value = stoi(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value " + str);
    }

    return TError::Success();
}

TError StringToOct(const std::string &str, unsigned &value) {
    try {
        value = stoul(str, nullptr, 8);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value " + str);
    }
    return TError::Success();
}

TError StringToDouble(const std::string &str, double &value) {
    try {
        value = stof(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad double value " + str);
    }
    return TError::Success();
}


TError StringToValue(const std::string &str, double &value, std::string &unit) {
    const char *ptr = str.c_str();
    char *end;

    value = strtod(ptr, &end);
    if (end == ptr)
        return TError(EError::InvalidValue, "Bad value: " + str);

    while (isblank(*end))
        end++;
    size_t len = strlen(end);
    while (len && isblank(end[len-1]))
        len--;
    unit = std::string(end, len);
    return TError::Success();
}

static char size_unit[] = {'B', 'K', 'M', 'G', 'T', 'P', 'E', 0};

TError StringToSize(const std::string &str, uint64_t &size) {
    std::string unit;
    double value;
    TError error;

    error = StringToValue(str, value, unit);
    if (error)
        return error;

    if (!unit[0]) {
        size = value;
        goto ok;
    }

    for (int i = 0; size_unit[i]; i++) {
        if (unit[0] == size_unit[i] ||
            unit[0] == tolower(size_unit[i])) {

            size = value * (1ull << (10 * i));

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
    return TError::Success();
}

std::string StringFormatSize(uint64_t value)
{
    int i = 0;

    while (value >= (1ull<<(10*(i+1))) && size_unit[i+1])
        i++;

    return StringFormat("%g%c", (double)value / (1ull<<(10*i)), size_unit[i]);
}

TError SplitString(const std::string &s, const char sep, std::vector<std::string> &tokens, size_t maxFields) {
    if (!maxFields)
        return TError(EError::Unknown, string(__func__) + ": invalid argument");

    try {
        istringstream ss(s);
        string tok;

        while(std::getline(ss, tok, sep)) {
            if (!--maxFields) {
                string rem;
                std::getline(ss, rem);
                if (rem.length()) {
                    tok += sep;
                    tok += rem;
                }
            }

            tokens.push_back(tok);
        }
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Can't split string");
    }

    return TError::Success();
}

TError SplitEscapedString(const std::string &s, const char sep, std::vector<std::string> &tokens) {
    stringstream ss;
    for (auto i = s.begin(); i != s.end(); i++) {
        if (*i == '\\' && (i + 1) != s.end() && *(i + 1) == sep) {
            ss << sep;
            i++;
        } else if (*i == sep) {
            if (ss.str().length())
                tokens.push_back(ss.str());
            ss.str("");
        } else {
            ss << *i;
        }
    }

    if (ss.str().length())
        tokens.push_back(ss.str());

    return TError::Success();
}

std::string MergeEscapeStrings(std::vector<std::string> &strings,
                               std::string sep, std::string rep) {
    std::stringstream str;
    bool first = true;

    for (auto s : strings) {
        if (!first)
            str << sep;
        first = false;
        str << StringReplaceAll(s, sep, rep);
    }

    return str.str();
}

std::string StringTrim(const std::string& s, const std::string &what) {
    std::size_t first = s.find_first_not_of(what);
    std::size_t last  = s.find_last_not_of(what);

    if (first == string::npos || last == string::npos)
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

    return str.substr(0, prefix.length()) == prefix;
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
