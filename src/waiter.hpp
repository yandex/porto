#pragma once

#include <string>
#include <vector>

#include "common.hpp"

class TClient;
class TContainer;

struct TContainerReport {
    TString Name;
    TString State;
    TString Label, Value;
    time_t When;

    TContainerReport(const TString &name, const TString &state, time_t when,
                     const TString &label, const TString &value):
        Name(name), State(state), Label(label), Value(value), When(when) {}
};

class TContainerWaiter : public std::enable_shared_from_this<TContainerWaiter> {
public:
    std::weak_ptr<TClient> Client;
    std::vector<TString> Names;
    std::vector<TString> Wildcards;
    std::vector<TString> Labels;
    bool Async;
    bool Active = false;

    TContainerWaiter(bool async) : Async(async) { }
    ~TContainerWaiter();

    void Activate(std::shared_ptr<TClient> &client);
    void Deactivate();

    bool ShouldReport(TContainer &ct);
    bool ShouldReportLabel(const TString &label);
    void Timeout();

    static void ReportAll(TContainer &ct, const TString &label = "", const TString &value = "");
};
