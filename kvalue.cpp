#include "kvalue.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"

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

TKeyValueStorage::TKeyValueStorage(const TMount &mount) :
    Tmpfs(mount) {
}

// use some forbidden character to represent slash in container name
const char SLASH_SUBST = '+';

string TKeyValueStorage::Name(const string &path) const {
    string s = path;
    for (string::size_type i = 0; i < s.length(); i++)
        if (s[i] == SLASH_SUBST)
            s[i] = '/';
    return s;
}

string TKeyValueStorage::Path(const string &name) const {
    string s = name;
    for (string::size_type i = 0; i < s.length(); i++)
        if (s[i] == '/')
            s[i] = SLASH_SUBST;
    return Tmpfs.GetMountpoint() + "/" + s;
}

void TKeyValueStorage::Merge(kv::TNode &node, kv::TNode &next) const {
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

TError TKeyValueStorage::LoadNode(const std::string &name, kv::TNode &node) const {
    int fd = open(Path(name).c_str(), O_RDONLY | O_CLOEXEC);
    node.Clear();
    TError error;
    try {
        google::protobuf::io::FileInputStream pist(fd);
        if (!ReadDelimitedFrom(&pist, &node)) {
            error = TError(EError::Unknown, "TKeyValueStorage: protobuf read error");
        }

        kv::TNode next;
        next.Clear();
        while (ReadDelimitedFrom(&pist, &next))
            Merge(node, next);
    } catch (...) {
        error = TError(EError::Unknown, "TKeyValueStorage: unhandled exception");
    }
    close(fd);
    return error;
}

TError TKeyValueStorage::AppendNode(const std::string &name, const kv::TNode &node) const {
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0755);
    TError error;

    if (lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        TError error(EError::Unknown, errno, "TKeyValueStorage open(" + Path(name) + ")");
        L_ERR() << "Can't append key-value node: " << error << std::endl;
        return error;
    }
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!WriteDelimitedTo(node, &post))
            error = TError(EError::Unknown, "TKeyValueStorage: protobuf write error");
    } catch (...) {
        error = TError(EError::Unknown, "TKeyValueStorage: unhandled exception");
    }
    close(fd);
    if (error)
        L_ERR() << "Can't append key-value node: " << error << std::endl;
    return error;
}

TError TKeyValueStorage::SaveNode(const std::string &name, const kv::TNode &node) const {
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755);
    TError error;
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!WriteDelimitedTo(node, &post))
            error = TError(EError::Unknown, "TKeyValueStorage: protobuf write error");
    } catch (...) {
        error = TError(EError::Unknown, "TKeyValueStorage: unhandled exception");
    }
    close(fd);
    return error;
}

TError TKeyValueStorage::RemoveNode(const std::string &name) const {
    TFile node(Path(name));
    return node.Remove();
}

TError TKeyValueStorage::MountTmpfs() {
    TMountSnapshot ms;

    set<shared_ptr<TMount>> mounts;
    TError error = ms.Mounts(mounts);
    if (error) {
        L_ERR() << "Can't create mount snapshot: " << error << std::endl;
        return error;
    }

    TFolder dir(Tmpfs.GetMountpoint());
    for (auto m : mounts)
        if (m->GetMountpoint() == Tmpfs.GetMountpoint()) {
            // FIXME: remove when all users are updated to this version
            // make sure permissions of existing directory are correct
            error = dir.GetPath().Chmod(config().keyval().file().perm());
            if (!error && dir.GetPath() == "/run/porto/kvs") {
                TFolder dir("/run/porto");
                error = dir.GetPath().Chmod(config().keyval().file().perm());
            }
            if (error)
                L_ERR() << error << ": can't change permissions of " << dir.GetPath().ToString() << std::endl;

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

TError TKeyValueStorage::ListNodes(std::vector<std::string> &list) const {
    vector<string> tmp;
    TFolder f(Tmpfs.GetMountpoint());
    TError error = f.Items(EFileType::Regular, tmp);
    if (error)
        return error;

    for (auto s : tmp)
        list.push_back(Name(s));

    return TError::Success();
}

TError TKeyValueStorage::Restore(std::map<std::string, kv::TNode> &map) const {
    std::vector<std::string> nodes;

    TError error = ListNodes(nodes);
    if (error) {
        L_ERR() << "Can't list key-value nodes: " << error << std::endl;
        return error;
    }

    for (auto &name : nodes) {
        kv::TNode node;
        node.Clear();

        L() << "Restoring " << name << " from key-value storage" << std::endl;

        TError error = LoadNode(name, node);
        if (error) {
            L_ERR() << "Can't load key-value nodes: " << error << std::endl;
            return error;
        }

        map[name] = node;
    }

    return TError::Success();
}

TError TKeyValueStorage::Dump() const {
    std::vector<std::string> nodes;

    TError error = ListNodes(nodes);
    if (error) {
        L() << "Can't list nodes: " << error.GetMsg() << std::endl;
        return error;
    }

    for (auto &name : nodes) {
        TError error;
        kv::TNode node;
        node.Clear();

        std::cout << name << ":" << std::endl;

        error = LoadNode(name, node);
        if (error) {
            L() << "Can't load node: " << error.GetMsg() << std::endl;
            continue;
        }

        for (int i = 0; i < node.pairs_size(); i++) {
            auto key = node.pairs(i).key();
            auto value = node.pairs(i).val();

            std::cout << " " << key << " = " << value << std::endl;
        }
    }

    return TError::Success();
}

TError TKeyValueStorage::Destroy() {
    return Tmpfs.Umount();
}

TError TKeyValueStorage::Create(const std::string &name) const {
    kv::TNode node;
    return SaveNode(name, node);
}

TError TKeyValueStorage::Append(const std::string &name, const std::string& key, const std::string& value) const {
    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return AppendNode(name, node);
}
