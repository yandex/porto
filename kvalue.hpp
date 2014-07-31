#ifndef __KVALUE_HPP__
#define __KVALUE_HPP__

#include <fstream>
#include <sstream>
#include <string>
#include <map>

#include "mount.hpp"
#include "folder.hpp"

using namespace std;

class TKeyValueStorage {
    TMount tmpfs;

    string Path(string name) {
        return tmpfs.Mountpoint() + "/" + name;
    }

    string Path(string name, string key) {
        return Path(name) + "/" + key;
    }

public:
    TKeyValueStorage() : tmpfs("tmpfs", "/tmp/porto", "tmpfs", 0, {"size=32m"}) {}

    void MountTmpfs() {
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

    void CreateNode(string name) {
        TFolder node(Path(name));
        if (!node.Exists())
            node.Create();
    }

    void RemoveNode(string name) {
        TFolder node(Path(name));
        if (node.Exists())
            node.Remove(true);
    }

    void Save(string node, string key, string value) {
        ofstream out(Path(node, key), ofstream::trunc);
        if (!out.is_open())
            throw "Cannot open " + Path(node, key);

        out << value;
    }

    string Load(string node, string key) {
        ifstream in(Path(node, key));
        if (!in.is_open())
            throw "Cannot open " + Path(node, key);

        string ret;
        in >> ret;

        return ret;
    }
};

#endif
