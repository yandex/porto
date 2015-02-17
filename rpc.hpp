#pragma once

#include "rpc.pb.h"
#include "holder.hpp"
#include "util/cred.hpp"
#include "context.hpp"
#include "client.hpp"

void SendReply(std::shared_ptr<TClient> client, rpc::TContainerResponse &response);
bool HandleRpcRequest(TContext &context, const rpc::TContainerRequest &req,
                      rpc::TContainerResponse &rsp, std::shared_ptr<TClient> client);
