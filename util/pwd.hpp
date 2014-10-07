#ifndef __PWD_HPP__
#define __PWD_HPP__

#include <string>

#include "error.hpp"

class TUser {
    std::string Name;
    int Id;
public:
    TUser(const std::string &name) : Name(name), Id(0) {}
    TUser(const int id) : Name(""), Id(id) {}
    TError Load();
    std::string GetName();
    int GetId();
};

class TGroup {
    std::string Name;
    int Id;
public:
    TGroup(const std::string &name) : Name(name), Id(0) {}
    TGroup(const int id) : Name(""), Id(id) {}
    TError Load();
    std::string GetName();
    int GetId();
};

#endif
