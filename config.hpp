#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <string>

#include "common.hpp"
#include "config.pb.h"

class TConfig : public TNonCopyable {
    cfg::TCfg Cfg;

    const std::vector<std::string> ConfigFiles = { "/etc/portod.conf", "/etc/default/portod.conf" };

    void LoadDefaults();
    bool LoadFile(const std::string &path, bool silent);
public:
    TConfig() {}
    void Load(bool silent = false);
    int Test(const std::string &path);
    cfg::TCfg &operator()();
};

extern TConfig config;

#endif
