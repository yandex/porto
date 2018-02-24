#pragma once

#include "common.hpp"
#include "client.hpp"

struct TRequest {
    std::shared_ptr<TClient> Client;
    rpc::TContainerRequest Request;
};

void QueueRpcRequest(TRequest &req);

void HandleRpcRequest(const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client);
