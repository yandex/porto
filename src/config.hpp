#pragma once

#include <string>

#include "common.hpp"

#undef PROTOBUF_DEPRECATED
#define PROTOBUF_DEPRECATED __attribute__((deprecated))
#include "config.pb.h"
#undef PROTOBUF_DEPRECATED

class TConfig : public TNonCopyable {
    cfg::TCfg Cfg;

    void LoadDefaults();
    bool LoadFile(const std::string &path);
public:
    TConfig() {}
    void Load();
    int Test(const std::string &path);
    cfg::TCfg &operator()();
};

extern TConfig config;
