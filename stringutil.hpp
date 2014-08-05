#ifndef __STRINGUTIL_HPP__
#define __STRINGUTIL_HPP__

#include <string>
#include <vector>
#include <set>

std::string CommaSeparatedList(const std::vector<std::string> &list);
std::string CommaSeparatedList(const std::set<std::string> &list);

std::vector<int> PidsFromLine(const std::string line);

#endif
