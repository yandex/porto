#include <sstream>

#include "util/string.hpp"

using std::string;
using std::vector;
using std::set;
using std::istringstream;
using std::stringstream;

string CommaSeparatedList(const vector<string> &list) {
    string ret;
    for (auto c = list.begin(); c != list.end(); ) {
        ret += *c;
        if (++c != list.end())
            ret += ",";
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
            return TError(EError::Unknown, string(__func__) + ": Bad integer value");
        }
    }

    return TError::Success();
}

TError StringToUint32(const std::string &str, uint32_t &value) {
    try {
        value = stoul(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value");
    }

    return TError::Success();
}

TError StringToUint64(const std::string &str, uint64_t &value) {
    try {
        value = stoull(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value");
    }

    return TError::Success();
}

TError StringToInt64(const std::string &str, int64_t &value) {
    try {
        value = stoll(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value");
    }

    return TError::Success();
}

TError StringToInt(const std::string &str, int &value) {
    try {
        value = stoi(str);
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Bad integer value");
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
        return TError(EError::Unknown, string(__func__) + ": Bad integer value");
    }

    return TError::Success();
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

std::string StringTrim(const std::string& s, const std::string &what) {
    std::size_t first = s.find_first_not_of(what);
    std::size_t last  = s.find_last_not_of(what);

    if (first == string::npos || last == string::npos)
        return "";

    return s.substr(first, last - first + 1);
}

std::string ReplaceMultiple(const std::string &str, const char rc) {
    stringstream s;

    bool foundChar = false;
    for (auto c : str) {
        if (c == rc) {
            foundChar = true;
        } else {
            if (foundChar)
                s << rc;
            foundChar = false;
            s << c;
        }
    }

    if (foundChar)
        s << rc;

    return s.str();
}
