#pragma once

#include <string>

#include "common.hpp"

#undef PROTOBUF_DEPRECATED
#define PROTOBUF_DEPRECATED __attribute__((deprecated))
#include "config.pb.h"
#undef PROTOBUF_DEPRECATED

class TConfig : public TNonCopyable {
    cfg::TCfg Cfg;

    void ReadDefaults();
    bool ReadFile(const std::string &path);
public:
    TConfig() {}
    void Read();
    cfg::TCfg &operator()();
};

extern TConfig config;
