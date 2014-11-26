#ifndef __PWD_HPP__
#define __PWD_HPP__

#include <string>

#include "common.hpp"

class TUserEntry {
    NO_COPY_CONSTRUCT(TUserEntry);
protected:
    std::string Name;
    int Id;
public:
    TUserEntry(const std::string &name) : Name(name), Id(-1) {}
    TUserEntry(const int id) : Name(""), Id(id) {}
    std::string GetName();
    int GetId();
};

class TUser : public TUserEntry {
    NO_COPY_CONSTRUCT(TUser);
public:
    TUser(const std::string &name) : TUserEntry(name) {}
    TUser(const int id) : TUserEntry(id) {}
    TError Load();
};

class TGroup : public TUserEntry {
    NO_COPY_CONSTRUCT(TGroup);
public:
    TGroup(const std::string &name) : TUserEntry(name) {}
    TGroup(const int id) : TUserEntry(id) {}
    TError Load();
};

#endif
