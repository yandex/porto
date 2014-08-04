#ifndef __REGISTRY_HPP__
#define __REGISTRY_HPP__

#include <list>

template <class T>
class TRegistry {
    std::list<std::weak_ptr<T>> items;
    std::mutex mutex;

public:
    std::shared_ptr<T> GetInstance(const T &item) {
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

    friend ostream& operator<<(std::ostream& os, const TRegistry<T> &r) {
        std::lock_guard<std::mutex> lock(mutex);

        for (auto m : r.items)
            os << m.use_count() << " " << *m.lock() << std::endl;

        return os;
    }

};

#endif
