#pragma once

#include <string>
#include <vector>

#include "common.hpp"

class TClient;
class TContainer;

struct TContainerReport {
    std::string Name;
    std::string State;
    time_t When;

    TContainerReport(const std::string &name, const std::string &state, time_t when):
        Name(name), State(state), When(when) {}
};

class TContainerWaiter : public std::enable_shared_from_this<TContainerWaiter> {
public:
    std::weak_ptr<TClient> Client;
    std::vector<std::string> Names;
    std::vector<std::string> Wildcards;
    bool Async;
    bool Active = false;

    TContainerWaiter(bool async) : Async(async) { }
    ~TContainerWaiter();

    void Activate(std::shared_ptr<TClient> &client);
    void Deactivate();

    bool ShouldReport(TContainer &ct);
    void Timeout();

    static void ReportAll(TContainer &ct);
};
