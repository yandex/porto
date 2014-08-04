#include <sstream>

#include "mount.hpp"
#include "file.hpp"
#include "registry.hpp"

using namespace std;

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

    for (auto line : f.AsLines()) {
        TMount m(line);
        mounts.insert(TRegistry<TMount>::Get(m));
    }
}

set<shared_ptr<TMount> > const& TMountState::Mounts() {
    return mounts;
}

ostream& operator<<(ostream& os, const TMountState& ms) {
    for (auto m : ms.mounts)
        os << *m << endl;

    return os;
}

void dump_reg(void) {
    cerr << TRegistry<TMount>::GetInstance() << endl;
}
