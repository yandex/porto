#ifndef __KVALUE_HPP__
#define __KVALUE_HPP__

#include <string>
#include <vector>

#include "kv.pb.h"
#include "error.hpp"
#include "util/mount.hpp"

class TKeyValueStorage {
    TMount Tmpfs;
    std::string Path(const std::string &name) const;
    void Merge(kv::TNode &node, kv::TNode &next) const;
    TError ListNodes(std::vector<std::string> &list) const;

    TKeyValueStorage(const TKeyValueStorage &) = delete;
    TKeyValueStorage &operator=(const TKeyValueStorage &) = delete;

public:
    TError MountTmpfs();

    TKeyValueStorage();
    TError LoadNode(const std::string &name, kv::TNode &node) const;
    TError SaveNode(const std::string &name, const kv::TNode &node) const;
    TError AppendNode(const std::string &name, const kv::TNode &node) const;
    TError RemoveNode(const std::string &name) const;
    TError Restore(std::map<std::string, kv::TNode> &map) const;
    TError Dump() const;
};

#endif
