#include <sstream>

#include "stringutil.hpp"

using namespace std;

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
            return TError("Bad integer value");
        }
    }

    return TError();
}

TError StringToUint64(const std::string &string, uint64_t &value) {
    try {
        value = stoull(string);
    } catch (...) {
        return TError("Bad integer value");
    }

    return TError();
}
