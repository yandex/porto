#pragma once

#include "util/log.hpp"

class TTaskEnv;

class TContainerValue {
protected:
    const char *Name;
    const char *Desc;
    const std::set<EContainerState> State;
    std::weak_ptr<TContainer> Container;

    TContainerValue(
           const char *name,
           const char *desc,
           const std::set<EContainerState> &state) :
        Name(name), Desc(desc), State(state) {}

    std::shared_ptr<TContainer> GetContainer() const;

public:
    void SetContainer(std::shared_ptr<TContainer> container) {
        Container = container;
    }
    const char *GetName() const { return Name; }
    const char *GetDesc() const { return Desc; }
    const std::set<EContainerState> &GetState() const { return State; }
    virtual TError PrepareTaskEnv(TTaskEnv &taskEnv) { return TError::Success(); }
};

class TRawValueMap;
class TValue;

void AddContainerValue(std::shared_ptr<TRawValueMap> m, std::shared_ptr<TContainer> c, TValue *av);
TContainerValue *ToContainerValue(TValue *av);
