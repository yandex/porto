#pragma once

#include "rpc.pb.h"
#include "context.hpp"
#include "client.hpp"

void HandleRpcRequest(TContext &context, const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client);
