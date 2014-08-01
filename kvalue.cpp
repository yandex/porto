#include "kvalue.hpp"

#include "folder.hpp"
#include "file.hpp"

using namespace std;

TKeyValueStorage::TKeyValueStorage() :
    tmpfs("tmpfs", "/tmp/porto", "tmpfs", 0, {"size=32m"}) {
}

string TKeyValueStorage::Path(string name) {
    return tmpfs.Mountpoint() + "/" + name;
}

string TKeyValueStorage::Path(string name, string key) {
    return Path(name) + "/" + key;
}

bool TKeyValueStorage::ValidName(string name) {
    if (name.size() > 0 && name[0] != '.')
        return true;

    return false;
}

string TKeyValueStorage::RemovingName(string name) {
    return "." + name;
}

void TKeyValueStorage::MountTmpfs() {
    TMountState ms;
    ms.UpdateFromProcfs();

    for (auto m : ms.Mounts())
        if (m->Mountpoint() == tmpfs.Mountpoint())
            return;

    TFolder mnt(tmpfs.Mountpoint());
    if (!mnt.Exists())
        mnt.Create();

    tmpfs.Mount();
}

void TKeyValueStorage::CreateNode(string name) {
    if (!ValidName(name))
        throw "Invalid node name";

    TFolder node(Path(name));
    if (!node.Exists())
        node.Create();
}

void TKeyValueStorage::RemoveNode(string name) {
    TFolder node(Path(name));
    if (node.Exists()) {
        // rename to .name to be atomic
        node.Rename(RemovingName(name));
        node.Remove(true);
    }
}

void TKeyValueStorage::Save(string node, string key, string value) {
    TFile f(Path(node, key));

    f.WriteStringNoAppend(value);
}

string TKeyValueStorage::Load(string node, string key) {
    TFile f(Path(node, key));

    return f.AsString();
}

std::vector<std::string> TKeyValueStorage::ListNodes() {
    TFolder f(tmpfs.Mountpoint());

    return f.Items(TFile::Directory);
}

std::vector<std::string> TKeyValueStorage::ListKeys(std::string node) {
    TFolder f(Path(node));

    return f.Items(TFile::Regular);
}
