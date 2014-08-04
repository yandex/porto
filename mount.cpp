#include <sstream>

#include "mount.hpp"
#include "file.hpp"

using namespace std;

TMountRegistry registry;

// from single /proc/self/mounts line, like:
// /dev/sda1 /boot ext4 rw,seclabel,relatime,data=ordered 0 0
TMount::TMount(string mounts_line) {
    istringstream ss(mounts_line);
    string flag_string, t;
    ss >> device >> mountpoint >> vfstype >> flag_string;

    for (auto i = flag_string.begin(); i != flag_string.end(); i++) {
        if (*i == ',' && !t.empty()) {
            flags.insert(t);
            t.clear();
        } else
            t += *i;
    }

    if (!t.empty())
        flags.insert(t);
}

TMountState::TMountState() {
    TFile f("/proc/self/mounts");

    for (auto line : f.AsLines())
        mounts.insert(registry.GetMount(line));
}

set<shared_ptr<TMount> > const& TMountState::Mounts() {
    return mounts;
}

ostream& operator<<(ostream& os, const TMountState& ms) {
    for (auto m : ms.mounts)
        os << *m << endl;

    return os;
}

shared_ptr<TMount> TMountRegistry::GetMount(string mounts_line) {
    auto tmp = make_shared<TMount>(mounts_line);

    for (auto mount : mounts) {
        auto m = mount.lock();
        if (*tmp == *m)
            return m;
    }

    return tmp;
}
