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

vector<int> PidsFromLine(const std::string line) {
    vector<int> ret;

    istringstream iss(line);
    while (iss) {
        int p;
        iss >> p;
        ret.push_back(p);
    }

    return ret;
}
