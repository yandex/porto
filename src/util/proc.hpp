#pragma once

#include "string.hpp"

class TVmStat {
public:
   TUintMap Stat;

   TVmStat();
   void Reset();
   TError Parse(pid_t pid);
   void Add(const TVmStat &a);
   void Dump(rpc::TVmStat &s);
};

TError GetFdSize(pid_t pid, uint64_t &fdSize);
