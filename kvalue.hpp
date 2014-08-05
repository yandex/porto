#ifndef __KVALUE_HPP__
#define __KVALUE_HPP__

#include <string>
#include <vector>

#include "mount.hpp"
#include "kv.pb.h"
#include "error.hpp"

class TKeyValueStorage {
    TMount tmpfs;
    std::string Path(std::string name);

public:
    void MountTmpfs();

    TKeyValueStorage();
    TError LoadNode(std::string name, kv::TNode &node);
    TError SaveNode(std::string name, const kv::TNode &node);
    void RemoveNode(std::string name);
};

#endif
