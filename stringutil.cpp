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

std::vector<int> StringsToIntegers(std::vector<std::string> lines) {
    vector<int> ret;

    for (auto l : lines)
        ret.push_back(stoi(l));

    return ret;
}
