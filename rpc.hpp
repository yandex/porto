#ifndef __RPC_HPP__
#define __RPC_HPP__

#include "rpc.pb.h"
#include "holder.hpp"

rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req,
                 int uid, int gid);

#endif
