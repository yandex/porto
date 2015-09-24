#include <sstream>
#include <iomanip>

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

TError StringToUint16(const std::string &str, uint16_t &value) {
    uint32_t uint32;
    TError error = StringToUint32(str, uint32);
    if (error)
        return error;

    if (uint32 >= 1 << 16)
        return TError(EError::Unknown, string(__func__) + ": Integer value out of range");

    value = (uint16_t)uint32;
    return TError::Success();
}

TError StringToUint32(const std::string &str, uint32_t &value) {
    try {
        value = stoul(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value " + str);
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

TError StringWithUnitToUint64(const std::string &str, uint64_t &value) {
    try {
        size_t pos = 0;
        value = stoull(str, &pos);
        if (pos > 0 && pos < str.length()) {
            switch (str[pos]) {
            case 'G':
            case 'g':
                value <<= 10;
            case 'M':
            case 'm':
                value <<= 10;
            case 'K':
            case 'k':
                value <<= 10;
            default:
                break;
            }
        }
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value " + str);
    }

    return TError::Success();
}

std::string StringWithUnit(uint64_t value, int precision)
{
    std::ostringstream ret;
    double val = value / 1024;

    if (value < 1024) {
        ret << value << "B";
    } else if (value <= 1024ull * 1024) {
        ret << std::fixed << std::setprecision(precision) << val << "K";
    } else if (value <= 1024ull * 1024 * 1024) {
        val /= 1024;
        ret << std::fixed << std::setprecision(precision) << val << "M";
    } else if (value <= 1024ull * 1024 * 1024 * 1024) {
        val /= 1024 * 1024;
        ret << std::fixed << std::setprecision(precision) << val << "G";
    } else {
        val /= 1024ull * 1024 * 1024;
        ret << std::fixed << std::setprecision(precision) << val << "T";
    }

    return ret.str();
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

bool StringEndsWith(const std::string &str, const std::string &prefix) {
    if (str.length() < prefix.length())
        return false;

    return str.substr(str.length() - prefix.length(), prefix.length()) == prefix;
}

std::string MapToStr(const std::map<std::string, uint64_t> &m) {
    std::stringstream ss;
    for (auto pair : m) {
        if (ss.str().length())
            ss << " ";
        ss << pair.first << ": " << pair.second;
    }
    return ss.str();
}
