#ifndef __PWD_HPP__
#define __PWD_HPP__

#include <string>

#include "error.hpp"

class TUserEntry {
protected:
    std::string Name;
    int Id;
public:
    TUserEntry(const std::string &name) : Name(name), Id(0) {}
    TUserEntry(const int id) : Name(""), Id(id) {}
    std::string GetName();
    int GetId();
};

class TUser : public TUserEntry {
public:
    TUser(const std::string &name) : TUserEntry(name) {}
    TUser(const int id) : TUserEntry(id) {}
    TError Load();
};

class TGroup : public TUserEntry {
public:
    TGroup(const std::string &name) : TUserEntry(name) {}
    TGroup(const int id) : TUserEntry(id) {}
    TError Load();
};

#endif
