#ifndef __RPC_HPP__
#define __RPC_HPP__

#include "rpc.pb.h"
#include "holder.hpp"
#include "util/cred.hpp"
#include "context.hpp"

bool HandleRpcRequest(TContext &context, const rpc::TContainerRequest &req,
                      rpc::TContainerResponse &rsp, const TCred &cred);
#endif
