#ifndef __REGISTRY_HPP__
#define __REGISTRY_HPP__

#include <list>
#include <iostream>
#include <memory>

template <class T>
class TRegistry {
    std::list<std::weak_ptr<T>> items;

    TRegistry() {};
    TRegistry(const TRegistry &);
    void operator=(const TRegistry &);

public:
    static TRegistry &GetInstance() {
        static TRegistry instance;
        return instance;
    }

    std::shared_ptr<T> GetItem(const T &item) {
        items.remove_if([] (std::weak_ptr<T> i) { return i.expired(); });

        for (auto i : items) {
            if (auto il = i.lock()) {
                if (item == *il)
                    return il;
            }
        }

        auto n = std::make_shared<T>(item);
        items.push_back(n);
        n->SetNeedCleanup();

        return n;
    }

    static std::shared_ptr<T> Get(const T &item) {
        return TRegistry<T>::GetInstance().GetItem(item);
    }
};

#endif
