#include "kvalue.hpp"
#include "kv.pb.h"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

void TKeyValueNode::Merge(kv::TNode &node, kv::TNode &next) const {
    for (int i = 0; i < next.pairs_size(); i++) {
        auto key = next.pairs(i).key();
        auto value = next.pairs(i).val();

        bool replaced = false;
        for (int j = 0; j < node.pairs_size(); j++)
            if (key == node.pairs(j).key()) {
                node.mutable_pairs(j)->set_val(value);
                replaced = true;
                break;
            }

        if (!replaced) {
            auto pair = node.add_pairs();
            pair->set_key(key);
            pair->set_val(value);
        }
    }
}

#define __class__ (std::string(typeid(*this).name()))

TError TKeyValueNode::Load(kv::TNode &node) const {
    auto lock = Storage->ScopedLock();
    auto Path = GetPath();

    TScopedFd fd;
    fd = open(Path.ToString().c_str(), O_RDONLY | O_CLOEXEC);
    node.Clear();
    TError error;
    try {
        google::protobuf::io::FileInputStream pist(fd.GetFd());
        if (!ReadDelimitedFrom(&pist, &node)) {
            error = TError(EError::Unknown, __class__ + ": protobuf read error");
        }

        kv::TNode next;
        next.Clear();
        while (ReadDelimitedFrom(&pist, &next))
            Merge(node, next);
    } catch (...) {
        if (config().daemon().debug())
            throw;
        error = TError(EError::Unknown, __class__ + ": unhandled exception");
    }
    return error;
}

TError TKeyValueNode::Append(const kv::TNode &node) const {
    auto lock = Storage->ScopedLock();
    auto Path = GetPath();

    TScopedFd fd;
    fd = open(Path.ToString().c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    TError error;

    if (lseek(fd.GetFd(), 0, SEEK_END) < 0) {
        TError error(EError::Unknown, errno, __class__ + ": open(" + Path.ToString() + ")");
        L_ERR() << "Can't append key-value node: " << error << std::endl;
        return error;
    }
    try {
        google::protobuf::io::FileOutputStream post(fd.GetFd());
        if (!WriteDelimitedTo(node, &post))
            error = TError(EError::Unknown, __class__ + ": protobuf write error");
    } catch (...) {
        if (config().daemon().debug())
            throw;
        error = TError(EError::Unknown, __class__ + ": unhandled exception");
    }
    if (error)
        L_ERR() << "Can't append key-value node: " << error << std::endl;
    return error;
}

TError TKeyValueNode::Save(const kv::TNode &node) const {
    auto lock = Storage->ScopedLock();
    auto Path = GetPath();

    TScopedFd fd;
    fd = open(Path.ToString().c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    TError error;
    try {
        google::protobuf::io::FileOutputStream post(fd.GetFd());
        if (!WriteDelimitedTo(node, &post))
            error = TError(EError::Unknown, __class__ + ": protobuf write error");
    } catch (...) {
        if (config().daemon().debug())
            throw;
        error = TError(EError::Unknown, __class__ + ": unhandled exception");
    }
    return error;
}

TError TKeyValueNode::Remove() const {
    auto lock = Storage->ScopedLock();

    return GetPath().Unlink();
}

TError TKeyValueStorage::MountTmpfs(std::string size) {
    TError error;
    TMount mount;

    if (!Root.IsDirectory()) {
        if (Root.Exists())
            (void)Root.Unlink();
        error = Root.MkdirAll(config().keyval().file().perm());
        if (error) {
            L_ERR() << "Can't create key-value mount point: " << error << std::endl;
            return error;
        }
    }

    error = mount.Find(Root);
    if (error || mount.GetMountpoint() != Root) {
        error = Root.Mount("tmpfs", "tmpfs",
                           MS_NOEXEC | MS_NOSUID | MS_NODEV,
                           { size });
        if (error)
            L_ERR() << "Can't mount key-value tmpfs: " << error << std::endl;
    }

    return error;
}

std::shared_ptr<TKeyValueNode> TKeyValueStorage::GetNode(const std::string &name) {
    return std::make_shared<TKeyValueNode>(shared_from_this(), name);
}

std::shared_ptr<TKeyValueNode> TKeyValueStorage::GetNode(uint64_t id) {
    return GetNode(std::to_string(id));
}

TError TKeyValueStorage::ListNodes(std::vector<std::shared_ptr<TKeyValueNode>> &list) {
    std::vector<std::string> names;
    TError error;

    error = Root.ReadDirectory(names);
    if (error)
        return error;

    for (auto &name: names)
        list.push_back(GetNode(name));

    return TError::Success();
}

TError TKeyValueStorage::Dump() {
    std::vector<std::shared_ptr<TKeyValueNode>> nodes;

    TError error = ListNodes(nodes);
    if (error) {
        L_ERR() << "Can't list nodes: " << error.GetMsg() << std::endl;
        return error;
    }

    for (auto &n : nodes) {
        TError error;
        kv::TNode node;
        node.Clear();

        L() << n->Name << ":" << std::endl;

        error = n->Load(node);
        if (error) {
            L_ERR() << "Can't load node: " << error.GetMsg() << std::endl;
            continue;
        }

        for (int i = 0; i < node.pairs_size(); i++) {
            auto key = node.pairs(i).key();
            auto value = node.pairs(i).val();

            L() << " " << key << " = " << value << std::endl;
        }
    }

    return TError::Success();
}

TError TKeyValueStorage::Destroy() {
    return Root.Umount(UMOUNT_NOFOLLOW);
}

TError TKeyValueNode::Create() const {
    if (config().log().verbose())
        L_ACT() << "Create key-value node " << Name << std::endl;

    kv::TNode node;
    return Save(node);
}

TError TKeyValueNode::Append(const std::string& key, const std::string& value) const {
    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    if (config().log().verbose())
        L_ACT() << "Append " << key << "=" << value << " to key-value node " << Name << std::endl;

    return Append(node);
}

TError TKeyValueStorage::Get(const kv::TNode &node, const std::string &name, std::string &val) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        if (key == name) {
            val = node.pairs(i).val();
            return TError::Success();
        }
    }

    return TError(EError::Unknown, "Entry " + name + " not found");
}
