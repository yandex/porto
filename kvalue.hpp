#ifndef __KVALUE_HPP__
#define __KVALUE_HPP__

#include <string>
#include <vector>

#include "kv.pb.h"
#include "error.hpp"
#include "util/mount.hpp"

class TKeyValueStorage {
    TMount Tmpfs;
    std::string Path(const std::string &name);
    void Merge(kv::TNode &node, kv::TNode &next);
    std::vector<std::string> ListNodes();
    TError ListNodes(std::vector<std::string> &list);

public:
    TError MountTmpfs();

    TKeyValueStorage();
    TError LoadNode(const std::string &name, kv::TNode &node);
    TError SaveNode(const std::string &name, const kv::TNode &node);
    TError AppendNode(const std::string &name, const kv::TNode &node);
    TError RemoveNode(const std::string &name);
    TError Restore(std::map<std::string, kv::TNode> &map);
    TError Dump();
};

#endif
