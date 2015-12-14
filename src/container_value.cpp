#include "container.hpp"
#include "container_value.hpp"
#include "value.hpp"

void AddContainerValue(std::shared_ptr<TRawValueMap> m, std::shared_ptr<TContainer> c, TValue *av) {
    auto cv = ToContainerValue(av);
    cv->SetContainer(c);
    m->Add(cv->GetName(), av);
}

std::shared_ptr<TContainer> TContainerValue::GetContainer() const {
    std::shared_ptr<TContainer> container = Container.lock();
    PORTO_ASSERT(container != nullptr);
    return container;
}

TContainerValue *ToContainerValue(TValue *av) {
    try {
        auto p = dynamic_cast<TContainerValue *>(av);
        if (!p)
            PORTO_RUNTIME_ERROR("Invalid value cast");
        return p;
    } catch (std::bad_cast &e) {
        PORTO_RUNTIME_ERROR(std::string("Invalid value cast: ") + e.what());
        return nullptr;
    }
}
