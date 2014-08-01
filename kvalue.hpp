#ifndef __KVALUE_HPP__
#define __KVALUE_HPP__

#include <string>
#include <vector>

#include "mount.hpp"

class TKeyValueStorage {
    TMount tmpfs;

    std::string Path(std::string name);
    std::string Path(std::string name, std::string key);

    static bool ValidName(std::string name);
    static std::string RemovingName(std::string name);

public:
    TKeyValueStorage();

    void MountTmpfs();

    void CreateNode(std::string name);
    void RemoveNode(std::string name);

    void Save(std::string node, std::string key, std::string value);
    std::string Load(std::string node, std::string key);

    std::vector<std::string> ListNodes();
    std::vector<std::string> ListKeys(std::string node);
};

#endif
