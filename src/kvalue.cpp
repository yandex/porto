#include "kvalue.hpp"
#include "kv.pb.h"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
}

using std::string;
using std::set;
using std::shared_ptr;
using std::vector;

// use some forbidden character to represent slash in container name
const char SLASH_SUBST = '+';

TPath TKeyValueStorage::ToPath(const std::string &name) const {
    std::string s = name;
    for (std::string::size_type i = 0; i < s.length(); i++)
        if (s[i] == '/')
            s[i] = SLASH_SUBST;
    return Tmpfs.GetMountpoint() / s;
}

std::string TKeyValueStorage::FromPath(const std::string &path) {
    std::string s = path;
    for (std::string::size_type i = 0; i < s.length(); i++)
        if (s[i] == SLASH_SUBST)
            s[i] = '/';
    return s;
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

    TScopedFd fd;
    fd = open(Path.ToString().c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0755);
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

    TScopedFd fd;
    fd = open(Path.ToString().c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755);
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

    TFile node(Path);
    return node.Remove();
}

TKeyValueStorage::TKeyValueStorage(const TMount &mount) :
    Tmpfs(mount), DirnameLen((Tmpfs.GetMountpoint().ToString() + "/").length()) {}

TError TKeyValueStorage::MountTmpfs() {
    vector<shared_ptr<TMount>> mounts;
    TError error = TMount::Snapshot(mounts);
    if (error) {
        L_ERR() << "Can't create mount snapshot: " << error << std::endl;
        return error;
    }

    TFolder dir(Tmpfs.GetMountpoint());
    for (auto m : mounts)
        if (m->GetMountpoint() == Tmpfs.GetMountpoint()) {
            // FIXME remove when all users are updated to v0.28
            // make sure permissions of existing directory are correct
            error = dir.GetPath().Chmod(config().keyval().file().perm());
            if (!error && dir.GetPath() == "/run/porto/kvs") {
                TFolder dir("/run/porto");
                error = dir.GetPath().Chmod(config().keyval().file().perm());
            }
            if (error)
                L_ERR() << error << ": can't change permissions of " << dir.GetPath() << std::endl;

            return TError::Success();
        }

    if (!dir.Exists()) {
        error = dir.Create(config().keyval().file().perm(), true);
        if (error) {
            L_ERR() << "Can't create key-value mount point: " << error << std::endl;
            return error;
        }
    }

    error = Tmpfs.Mount();
    if (error)
        L_ERR() << "Can't mount key-value tmpfs: " << error << std::endl;
    return error;
}

std::shared_ptr<TKeyValueNode> TKeyValueStorage::GetNode(const TPath &path) {
    PORTO_ASSERT(path.ToString().length() > DirnameLen);
    return std::make_shared<TKeyValueNode>(shared_from_this(), path, path.ToString().substr(DirnameLen));
}

std::shared_ptr<TKeyValueNode> TKeyValueStorage::GetNode(uint16_t id) {
    TPath path = ToPath(std::to_string(id));
    PORTO_ASSERT(path.ToString().length() > DirnameLen);
    return std::make_shared<TKeyValueNode>(shared_from_this(), path, path.ToString().substr(DirnameLen));
}

TError TKeyValueStorage::ListNodes(std::vector<std::shared_ptr<TKeyValueNode>> &list) {
    vector<string> tmp;
    TFolder f(Tmpfs.GetMountpoint());
    TError error = f.Items(EFileType::Regular, tmp);
    if (error)
        return error;

    for (auto s : tmp)
        list.push_back(GetNode(Tmpfs.GetMountpoint() / s));

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

        L() << n->GetName() << ":" << std::endl;

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
    return Tmpfs.Umount();
}

TError TKeyValueNode::Create() const {
    if (config().log().verbose())
        L_ACT() << "Create key-value node " << Path << std::endl;

    kv::TNode node;
    return Save(node);
}

TError TKeyValueNode::Append(const std::string& key, const std::string& value) const {
    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    if (config().log().verbose())
        L_ACT() << "Append " << key << "=" << value << " to key-value node " << Path << std::endl;

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
