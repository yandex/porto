#pragma once

#include "error.hpp"

#define noinline __attribute__((noinline))

class TNonCopyable {
protected:
    TNonCopyable() = default;
    ~TNonCopyable() = default;
private:
    TNonCopyable(TNonCopyable const&) = delete;
    TNonCopyable& operator= (TNonCopyable const&) = delete;
    TNonCopyable(TNonCopyable const&&) = delete;
    TNonCopyable& operator= (TNonCopyable const&&) = delete;
};

const std::string ROOT_CONTAINER = "/";
constexpr uint16_t ROOT_CONTAINER_ID = 1;
const std::string DOT_CONTAINER = ".";
constexpr uint16_t PORTO_ROOT_CONTAINER_ID = 3;
const std::string PORTO_ROOT_CONTAINER = "/porto";
const std::string PORTO_ROOT_CGROUP = "porto";
const std::string PORTO_DAEMON_CGROUP = "portod";

extern void AckExitStatus(int pid);
