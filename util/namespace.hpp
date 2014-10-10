#ifndef __NAMESPACE_HPP__
#define __NAMESPACE_HPP__

#include <map>
#include <string>

#include "porto.hpp"
#include "error.hpp"

class TNamespaceSnapshot {
    std::map<int,int> nsToFd;
    NO_COPY_CONSTRUCT(TNamespaceSnapshot);

public:
    TNamespaceSnapshot() {}
    ~TNamespaceSnapshot() { Destroy(); }
    TError Create(int pid);
    TError Attach();
    void Destroy();
};

#endif
