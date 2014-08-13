#include "kvalue.hpp"
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
    tmpfs("tmpfs", "/tmp/porto", "tmpfs", 0, {"size=32m"}) {
}

string TKeyValueStorage::Path(const string &name) {
    return tmpfs.Mountpoint() + "/" + name;
}

void TKeyValueStorage::Merge(kv::TNode &node, kv::TNode &next) {
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

TError TKeyValueStorage::LoadNode(const std::string &name, kv::TNode &node)
{
    int fd = open(Path(name).c_str(), O_RDONLY);
    node.Clear();
    TError error;
    try {
        google::protobuf::io::FileInputStream pist(fd);
        if (!ReadDelimitedFrom(&pist, &node)) {
            error = TError(TError::Unknown, "TKeyValueStorage: protobuf read error");
        }

        kv::TNode next;
        next.Clear();
        while (ReadDelimitedFrom(&pist, &next))
            Merge(node, next);
    } catch (...) {
        error = TError(TError::Unknown, "TKeyValueStorage: unhandled exception");
    }
    close(fd);
    return error;
}

TError TKeyValueStorage::AppendNode(const std::string &name, const kv::TNode &node)
{
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY, 0755);
    TError error;

    if (lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        TError error(TError::Unknown, errno);
        TLogger::LogError(error);
        return error;
    }
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!WriteDelimitedTo(node, &post))
            error = TError(TError::Unknown, "TKeyValueStorage: protobuf write error");
    } catch (...) {
        error = TError(TError::Unknown, "TKeyValueStorage: unhandled exception");
    }
    close(fd);
    if (error)
        TLogger::LogError(error);
    return error;
}

TError TKeyValueStorage::SaveNode(const std::string &name, const kv::TNode &node)
{
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
    TError error;
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!WriteDelimitedTo(node, &post))
            error = TError(TError::Unknown, "TKeyValueStorage: protobuf write error");
    } catch (...) {
        error = TError(TError::Unknown, "TKeyValueStorage: unhandled exception");
    }
    close(fd);
    return error;
}

TError TKeyValueStorage::RemoveNode(const std::string &name) {
    TFile node(Path(name));
    return node.Remove();
}

void TKeyValueStorage::MountTmpfs() {
    TMountSnapshot ms;

    for (auto m : ms.Mounts())
        if (m->Mountpoint() == tmpfs.Mountpoint())
            return;

    TFolder mnt(tmpfs.Mountpoint());
    if (!mnt.Exists())
        mnt.Create();

    TError error = tmpfs.Mount();
    TLogger::LogError(error, "Can't mount key-valye tmpfs");
}

TError TKeyValueStorage::ListNodes(std::vector<std::string> &list) {
    TFolder f(tmpfs.Mountpoint());
    return f.Items(TFile::Regular, list);
}

TError TKeyValueStorage::Restore(std::map<std::string, kv::TNode> &map) {
    std::vector<std::string> nodes;

    TError error = ListNodes(nodes);
    if (error) {
        TLogger::LogError(error);
        return error;
    }

    for (auto &name : nodes) {
        kv::TNode node;
        node.Clear();

        TError error = LoadNode(name, node);
        if (error) {
            TLogger::LogError(error);
            return error;
        }

        map[name] = node;
    }

    return TError();
}

TError TKeyValueStorage::Dump() {
    std::vector<std::string> nodes;

    TError error = ListNodes(nodes);
    if (error) {
        cerr << "Can't list nodes: " << error.GetMsg() << endl;
        return error;
    }

    for (auto &name : nodes) {
        TError error;
        kv::TNode node;
        node.Clear();

        cout << name << ":" << endl;

        error = LoadNode(name, node);
        if (error) {
            cerr << "Can't load node: " << error.GetMsg() << endl;
            continue;
        }

        for (int i = 0; i < node.pairs_size(); i++) {
            auto key = node.pairs(i).key();
            auto value = node.pairs(i).val();

            cout << " " << key << " = " << value << endl;
        }
    }

    return TError();
}
