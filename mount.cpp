#include <sstream>
#include <fstream>

#include "mount.hpp"

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
    ifstream s("/proc/self/mounts");
    string line;

    if (!s.is_open())
        throw "Cannot open /proc/self/mounts";

    while (getline(s, line)) {
        TMount *mount = new TMount(line);
        mounts.insert(mount);
    }
}
