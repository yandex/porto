#pragma once

#include "common.hpp"
#include "client.hpp"

void HandleRpcRequest(const rpc::TContainerRequest &req,
		      std::shared_ptr<TClient> client);
