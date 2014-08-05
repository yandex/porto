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

string TKeyValueStorage::Path(string name) {
    return tmpfs.Mountpoint() + "/" + name;
}

TError TKeyValueStorage::LoadNode(std::string name, kv::TNode &node)
{
    int fd = open(Path(name).c_str(), O_RDONLY);
    node.Clear();
    TError error;
    try {
        google::protobuf::io::FileInputStream pist(fd);
        if (!readDelimitedFrom(&pist, &node)) {
            error = TError("protobuf read error");
        }
    } catch (...) {
        error = TError("unhandled exception");
    }
    close(fd);
    return error;
}

TError TKeyValueStorage::SaveNode(std::string name, const kv::TNode &node)
{
    int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
    TError error;
    try {
        google::protobuf::io::FileOutputStream post(fd);
        if (!writeDelimitedTo(node, &post))
            error = TError("protobuf write error");
    } catch (...) {
        error = TError("unhandled exception");
    }
    close(fd);
    return error;
}

void TKeyValueStorage::RemoveNode(std::string name) {
    TFile node(Path(name));
    node.Remove();
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
