#ifndef __SUBSYSTEM_HPP__
#define __SUBSYSTEM_HPP__

#include <ostream>
#include <string>
#include <memory>

class TSubsystem {
    std::string name;

public:
    static std::shared_ptr<TSubsystem> Memory();
    static std::shared_ptr<TSubsystem> Freezer();
    static std::shared_ptr<TSubsystem> Cpu();
    
    TSubsystem(std::string name);
    std::string Name();

    friend bool operator==(const TSubsystem& c1, const TSubsystem& c2) {
        return c1.name == c2.name;
    }

    friend std::ostream& operator<<(std::ostream& os, const TSubsystem& cg) {
        return (os << cg.name);
    }
};

#endif
