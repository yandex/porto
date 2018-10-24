#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "container.hpp"

class TClient;

struct TContainerReport {
    std::string Name;
    EContainerState State;
    std::string Label, Value;
    time_t When;

    TContainerReport(const std::string &name, EContainerState state, time_t when,
                     const std::string &label, const std::string &value):
        Name(name), State(state), Label(label), Value(value), When(when) {}
};

class TContainerWaiter : public std::enable_shared_from_this<TContainerWaiter> {
public:
    TClient *Client = nullptr;
    std::vector<std::string> Names;
    std::vector<std::string> Wildcards;
    std::vector<std::string> Labels;
    bool Async;

    TContainerWaiter(bool async) : Async(async) { }
    ~TContainerWaiter();

    void Activate(TClient &client);
    void Deactivate();
    void DeactivateLocked();

    bool ShouldReport(TContainer &ct);
    bool ShouldReportLabel(const std::string &label);
    void Timeout();

    static void ReportAll(TContainer &ct, const std::string &label = "", const std::string &value = "");
};
