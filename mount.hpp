#include <string>
#include <set>

#include <sys/mount.h>

using namespace std;

class TMount {
    string device;
    string mountpoint;
    string vfstype;
    set<string> flags;

    unsigned long mountflags = 0;

public:
    TMount(string mounts_line);

    TMount(string device, string mountpoint, string vfstype,
           unsigned long mountflags, set<string> flags) :
        device (device), mountpoint (mountpoint), vfstype (vfstype),
        flags (flags), mountflags (mountflags) {
    }

    friend ostream& operator<<(ostream& os, const TMount& m) {
        os << m.device << " " << m.mountpoint << " ";
        for (auto f : m.flags)
            os << f << " ";

        return os;
    }

    const string Mountpoint() {
        return mountpoint;
    }

    set<string> const& Flags() {
        return flags;
    }

    string CommaDelimitedFlags() {
        string ret;
        for (auto c = flags.begin(); c != flags.end(); ) {
            ret += *c;
            if (++c != flags.end())
                ret += ",";
        }
        return ret;
    }

    void Mount() {
        if (mount(device.c_str(), mountpoint.c_str(), vfstype.c_str(),
                  mountflags, CommaDelimitedFlags().c_str())) {
            if (errno == EBUSY)
                throw "Already mounted";
            else
                throw "Cannot mount filesystem " + mountpoint;
        }
    }

    void Umount () {
        if (umount(mountpoint.c_str()))
            throw "Cannot umount filesystem " + mountpoint;
    }
};

class TMountState {
    set<TMount*> mounts;

public:
    TMountState();
    ~TMountState() {
        for (auto m : mounts)
            delete m;
    }

    set<TMount*> const& Mounts() {
        return mounts;
    }

    friend ostream& operator<<(ostream& os, const TMountState& ms) {
        for (auto m : ms.mounts)
            os << *m << endl;

        return os;
    }
};
