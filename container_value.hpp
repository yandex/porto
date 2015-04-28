#pragma once

#include "container.hpp"
#include "util/log.hpp"

class TContainerValue {
protected:
    const char *Name;
    const char *Desc;
    const std::set<EContainerState> State;
    std::weak_ptr<TContainer> Container;
    bool Implemented = true;

    TContainerValue(
           const char *name,
           const char *desc,
           const std::set<EContainerState> &state) :
        Name(name), Desc(desc), State(state) {}

    std::shared_ptr<TContainer> GetContainer() const {
        std::shared_ptr<TContainer> container = Container.lock();
        PORTO_ASSERT(container);
        return container;
    }

public:
    void SetContainer(std::shared_ptr<TContainer> container) {
        Container = container;
    }
    const char *GetName() { return Name; }
    const char *GetDesc() { return Desc; }
    const std::set<EContainerState> &GetState() { return State; }
    virtual TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) { return TError::Success(); }
    virtual bool IsImplemented() { return Implemented; }
};

class TRawValueMap;
class TAbstractValue;

void AddContainerValue(std::shared_ptr<TRawValueMap> m, std::shared_ptr<TContainer> c, TAbstractValue *av);
TContainerValue *ToContainerValue(TAbstractValue *av);
