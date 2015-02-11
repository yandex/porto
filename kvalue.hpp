#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "kv.pb.h"
#include "util/mount.hpp"

class TKeyValueNode : public TNonCopyable {
    const TPath Path;
    const std::string Name;
    void Merge(kv::TNode &node, kv::TNode &next) const;

public:
    TKeyValueNode(const TPath &path, const std::string &name) : Path(path), Name(name) {}

    TError Load(kv::TNode &node) const;
    TError Save(const kv::TNode &node) const;
    TError Append(const kv::TNode &node) const;
    TError Append(const std::string& key, const std::string& value) const;
    TError Remove() const;
    TError Create() const;
    const TPath &GetPath() const { return Path; }
    const std::string &GetName() const { return Name; }
};

class TKeyValueStorage : public TNonCopyable {
    TMount Tmpfs;
    TPath ToPath(const std::string &name) const;
    size_t DirnameLen;

public:
    TError MountTmpfs();

    TKeyValueStorage(const TMount &mount);

    std::shared_ptr<TKeyValueNode> GetNode(const std::string &path) const;
    std::shared_ptr<TKeyValueNode> GetNode(uint16_t id) const;
    TError Dump() const;
    TError ListNodes(std::vector<std::shared_ptr<TKeyValueNode>> &list) const;
    TError Destroy();
    std::string GetRoot() const { return Tmpfs.GetMountpoint() + "/"; }

    static TError Get(const kv::TNode &node, const std::string &name, std::string &val);
    static std::string FromPath(const std::string &path);
};
