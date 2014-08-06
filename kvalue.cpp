#include "kvalue.hpp"
#include "file.hpp"
#include "folder.hpp"
#include "protobuf.hpp"

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
        if (!readDelimitedFrom(&pist, &node)) {
            error = TError("TKeyValueStorage: protobuf read error");
        }

        kv::TNode next;
        next.Clear();
        while (readDelimitedFrom(&pist, &next))
            Merge(node, next);
    } catch (...) {
        error = TError("TKeyValueStorage: unhandled exception");
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
        return TError(errno);
    }
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!writeDelimitedTo(node, &post))
            error = TError("TKeyValueStorage: protobuf write error");
    } catch (...) {
        error = TError("TKeyValueStorage: unhandled exception");
    }
    close(fd);
    return error;
}

TError TKeyValueStorage::SaveNode(const std::string &name, const kv::TNode &node)
{
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
    TError error;
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!writeDelimitedTo(node, &post))
            error = TError("TKeyValueStorage: protobuf write error");
    } catch (...) {
        error = TError("TKeyValueStorage: unhandled exception");
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

    tmpfs.Mount();
}

std::vector<std::string> TKeyValueStorage::ListNodes() {
    TFolder f(tmpfs.Mountpoint());
    return f.Items(TFile::Regular);
}

std::map<std::string, kv::TNode> TKeyValueStorage::Restore() {
    std::map<std::string, kv::TNode> map;

    for (auto &name : ListNodes()) {
        TError error;
        kv::TNode node;
        node.Clear();

        error = LoadNode(name, node);
        if (error) {
            // TODO: does it make sense to report to upper layer?
            TLogger::LogError(error);
            continue;
        }

        map[name] = node;
    }

    return map;
}
