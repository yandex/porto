#pragma once

#include <string>
#include <vector>
#include <mutex>

#include "common.hpp"
#include "util/mount.hpp"
#include "util/locks.hpp"

class TKeyValueNode;

namespace kv {
    class TNode;
};

class TKeyValueStorage : public std::enable_shared_from_this<TKeyValueStorage>,
                         public TLockable, public TNonCopyable {

public:
    const TPath Root;
    TKeyValueStorage(const TPath &root) : Root(root) {}

    TError MountTmpfs(std::string size);

    std::shared_ptr<TKeyValueNode> GetNode(const std::string &name);
    std::shared_ptr<TKeyValueNode> GetNode(uint64_t id);
    TError Dump();
    TError ListNodes(std::vector<std::shared_ptr<TKeyValueNode>> &list);
    TError Destroy();

    static TError Get(const kv::TNode &node, const std::string &name, std::string &val);
};

class TKeyValueNode : public TNonCopyable {
    std::shared_ptr<TKeyValueStorage> Storage;

    void Merge(kv::TNode &node, kv::TNode &next) const;
    TPath GetPath() const { return Storage->Root / Name; }
public:
    const std::string Name;

    TKeyValueNode(std::shared_ptr<TKeyValueStorage> storage,
                  const std::string &name) :
        Storage(storage), Name(name) {}

    TError Load(kv::TNode &node) const;
    TError Save(const kv::TNode &node) const;
    TError Append(const kv::TNode &node) const;
    TError Append(const std::string& key, const std::string& value) const;
    TError Remove() const;
    TError Create() const;
};
