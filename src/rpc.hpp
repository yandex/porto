#pragma once

#include "common.hpp"

class TClient;

class TRequest {
public:
    std::shared_ptr<TClient> Client;
    rpc::TContainerRequest Req;

    uint64_t QueueTime;
    uint64_t StartTime;
    uint64_t FinishTime;

    bool RoReq;
    bool IoReq;
    bool VlReq;
    bool SecretReq;

    std::string Cmd;
    std::string Arg;
    std::string Opt;

    void Classify();
    void Parse();
    TError Check();
    void Handle();
    void ChangeId();
};

void StartRpcQueue();
void StopRpcQueue();
void QueueRpcRequest(std::unique_ptr<TRequest> &req);
uint64_t RpcRequestsTopRunningTime();
