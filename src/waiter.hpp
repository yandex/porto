#pragma once

#include <string>
#include <vector>

#include "common.hpp"

class TClient;
class TContainer;

struct TContainerReport {
    std::string Name;
    std::string State;
    std::string Label, Value;
    time_t When;

    TContainerReport(const std::string &name, const std::string &state, time_t when,
                     const std::string &label, const std::string &value):
        Name(name), State(state), Label(label), Value(value), When(when) {}
};

class TContainerWaiter : public std::enable_shared_from_this<TContainerWaiter> {
public:
    std::weak_ptr<TClient> Client;
    std::vector<std::string> Names;
    std::vector<std::string> Wildcards;
    std::vector<std::string> Labels;
    std::string TargetState;
    bool Async;
    bool Active = false;

    TContainerWaiter(bool async) : Async(async) { }
    ~TContainerWaiter();

    bool operator==(const TContainerWaiter &waiter) const;

    void Activate(std::shared_ptr<TClient> &client);
    void Deactivate();

    bool ShouldReport(TContainer &ct);
    bool ShouldReportLabel(const std::string &label);
    void Timeout();

    static void ReportAll(TContainer &ct, const std::string &label = "", const std::string &value = "");
    static TError Remove(const TContainerWaiter &waiter, const TClient &client);
};
