#ifndef __REGISTRY_HPP__
#define __REGISTRY_HPP__

#include <list>
#include <mutex>

template <class T>
class TRegistry {
    std::list<std::weak_ptr<T>> items;
    std::mutex mutex;

    TRegistry() {};
    TRegistry(const TRegistry &);
    void operato(const TRegistry &);

public:
    static TRegistry &GetInstance() {
        static TRegistry instance;
        return instance;
    }

    std::shared_ptr<T> GetItem(const T &item) {
        std::lock_guard<std::mutex> lock(mutex);

        items.remove_if([] (std::weak_ptr<T> i) { return i.expired(); });
        for (auto i : items) {
            if (auto il = i.lock()) {
                if (item == *il)
                    return il;
            }
        }

        auto n = make_shared<T>(item);
        items.push_back(n);

        return n;
    }

    static std::shared_ptr<T> Get(const T &item) {
        return TRegistry<T>::GetInstance().GetItem(item);
    }

    friend ostream& operator<<(std::ostream& os, TRegistry<T> &r) {
        std::lock_guard<std::mutex> lock(r.mutex);

        for (auto m : r.items)
            os << m.use_count() << " " << *m.lock() << std::endl;

        return os;
    }

};

#endif
