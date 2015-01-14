#ifndef __RPC_HPP__
#define __RPC_HPP__

#include "rpc.pb.h"
#include "holder.hpp"
#include "util/cred.hpp"

rpc::TContainerResponse
HandleRpcRequest(THolder &cholder, const rpc::TContainerRequest &req,
                 const TCred &cred);

#endif
