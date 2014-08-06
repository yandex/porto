#ifndef __KVALUE_HPP__
#define __KVALUE_HPP__

#include <string>
#include <vector>

#include "mount.hpp"
#include "kv.pb.h"
#include "error.hpp"

class TKeyValueStorage {
    TMount tmpfs;
    std::string Path(const std::string &name);

    void Merge(kv::TNode &node, kv::TNode &next);

public:
    void MountTmpfs();

    TKeyValueStorage();
    TError LoadNode(const std::string &name, kv::TNode &node);
    TError SaveNode(const std::string &name, const kv::TNode &node);
    TError AppendNode(const std::string &name, const kv::TNode &node);
    TError RemoveNode(const std::string &name);
    std::vector<std::string> ListNodes();
};

#endif
