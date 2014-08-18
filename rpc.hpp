#ifndef __RPC_HPP__
#define __RPC_HPP__

#include "rpc.pb.h"
#include "container.hpp"

rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req);

#endif
