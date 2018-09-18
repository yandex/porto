#pragma once

#include "common.hpp"

class TClient;

class TRequest {
public:
    std::shared_ptr<TClient> Client;
    rpc::TPortoRequest Req;

    uint64_t QueueTime;
    uint64_t StartTime;
    uint64_t FinishTime;

    bool RoReq;
    bool IoReq;

    TString Cmd;
    TString Arg;
    TString Opt;

    void Classify();
    void Parse();
    TError Check();
    void Handle();
};

void StartRpcQueue();
void StopRpcQueue();
void QueueRpcRequest(std::unique_ptr<TRequest> &req);
