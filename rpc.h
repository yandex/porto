#ifndef __RPC_H__
#define __RPC_H__

#include "rpc.pb.h"
#include "container.h"

// TODO: /run/porto.socket
#define RPC_SOCK_PATH "/tmp/porto.socket"

rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req);

#endif
