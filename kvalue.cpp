#include "porto.hpp"
#include "kvalue.hpp"
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

using namespace std;

TKeyValueStorage::TKeyValueStorage() :
    Tmpfs("tmpfs", KVALUE_ROOT, "tmpfs", {KVALUE_SIZE}) {
}

string TKeyValueStorage::Name(const string &path) const {
    string s = path;
    replace(s.begin(), s.end(), '.', '/');
    return s;
}

string TKeyValueStorage::Path(const string &name) const {
    string fileName = name;
    replace(fileName.begin(), fileName.end(), '/', '.');
    return Tmpfs.GetMountpoint() + "/" + fileName;
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
    int fd = open(Path(name).c_str(), O_RDONLY);
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
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY, 0755);
    TError error;

    if (lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        TError error(EError::Unknown, errno, "TKeyValueStorage open(" + Path(name) + ")");
        TLogger::LogError(error, "Can't append key-value node");
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
        TLogger::LogError(error, "Can't append key-value node");
    return error;
}

TError TKeyValueStorage::SaveNode(const std::string &name, const kv::TNode &node) const {
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
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
        TLogger::LogError(error, "Can't create mount snapshot");
        return error;
    }

    for (auto m : mounts)
        if (m->GetMountpoint() == Tmpfs.GetMountpoint())
            return TError::Success();

    TFolder dir(Tmpfs.GetMountpoint());
    if (!dir.Exists()) {
        error = dir.Create(KVS_PERM, true);
        TLogger::LogError(error, "Can't create key-value mount point");
        if (error)
            return error;
    }

    error = Tmpfs.Mount();
    TLogger::LogError(error, "Can't mount key-value tmpfs");
    return error;
}

TError TKeyValueStorage::ListNodes(std::vector<std::string> &list) const {
    vector<string> tmp;
    TFolder f(Tmpfs.GetMountpoint());
    TError error = f.Items(TFile::Regular, tmp);
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
        TLogger::LogError(error, "Can't list key-value nodes");
        return error;
    }

    for (auto &name : nodes) {
        kv::TNode node;
        node.Clear();

        TLogger::Log() << "Restoring " << name << " from key-value storage" << endl;

        TError error = LoadNode(name, node);
        if (error) {
            TLogger::LogError(error, "Can't load key-value node");
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
        TLogger::Log() << "Can't list nodes: " << error.GetMsg() << endl;
        return error;
    }

    for (auto &name : nodes) {
        TError error;
        kv::TNode node;
        node.Clear();

        cout << name << ":" << endl;

        error = LoadNode(name, node);
        if (error) {
            TLogger::Log() << "Can't load node: " << error.GetMsg() << endl;
            continue;
        }

        for (int i = 0; i < node.pairs_size(); i++) {
            auto key = node.pairs(i).key();
            auto value = node.pairs(i).val();

            cout << " " << key << " = " << value << endl;
        }
    }

    return TError::Success();
}

TError TKeyValueStorage::Destroy() {
    return Tmpfs.Umount();
}
