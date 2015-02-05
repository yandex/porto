#include "container.hpp"
#include "container_value.hpp"
#include "value.hpp"

void AddContainerValue(std::shared_ptr<TRawValueMap> m, std::shared_ptr<TContainer> c, TAbstractValue *av) {
    auto cv = ToContainerValue(av);
    cv->SetContainer(c);
    m->Add(cv->GetName(), av);
}

TContainerValue *ToContainerValue(TAbstractValue *av) {
    try {
        return dynamic_cast<TContainerValue *>(av);
    } catch (std::bad_cast &e) {
        PORTO_RUNTIME_ERROR(std::string("Invalid variant cast: ") + e.what());
        return nullptr;
    }
}
