#ifndef __RPC_HPP__
#define __RPC_HPP__

#include "rpc.pb.h"
#include "holder.hpp"
#include "util/cred.hpp"
#include "context.hpp"

rpc::TContainerResponse
HandleRpcRequest(TContext &context, const rpc::TContainerRequest &req,
                 const TCred &cred);

#endif
