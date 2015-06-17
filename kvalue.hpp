#pragma once

#include <string>
#include <vector>
#include <mutex>

#include "common.hpp"
#include "kv.pb.h"
#include "util/mount.hpp"

class TKeyValueStorage;

class TKeyValueNode : public TNonCopyable {
    std::shared_ptr<TKeyValueStorage> Storage;
    const TPath Path;
    const std::string Name;

    void Merge(kv::TNode &node, kv::TNode &next) const;

public:
    TKeyValueNode(std::shared_ptr<TKeyValueStorage> storage,
                  const TPath &path, const std::string &name) :
        Storage(storage), Path(path), Name(name) {}

    TError Load(kv::TNode &node) const;
    TError Save(const kv::TNode &node) const;
    TError Append(const kv::TNode &node) const;
    TError Append(const std::string& key, const std::string& value) const;
    TError Remove() const;
    TError Create() const;
    const TPath &GetPath() const { return Path; }
    const std::string &GetName() const { return Name; }
};

class TKeyValueStorage : public std::enable_shared_from_this<TKeyValueStorage>,
                         public TLockable<>, public TNonCopyable {
    const TMount Tmpfs;
    const size_t DirnameLen;

    TPath ToPath(const std::string &name) const;

public:
    TError MountTmpfs();

    TKeyValueStorage(const TMount &mount);

    std::shared_ptr<TKeyValueNode> GetNode(const std::string &path);
    std::shared_ptr<TKeyValueNode> GetNode(uint16_t id);
    TError Dump();
    TError ListNodes(std::vector<std::shared_ptr<TKeyValueNode>> &list);
    TError Destroy();
    std::string GetRoot() const { return Tmpfs.GetMountpoint() + "/"; }

    static TError Get(const kv::TNode &node, const std::string &name, std::string &val);
    static std::string FromPath(const std::string &path);
};
