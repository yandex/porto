#include <string>
#include <set>

using namespace std;

class TMount {
    string device;
    string mountpoint;
    string vfstype;
    set<string> flags;

public:
    TMount(string mounts_line);

    friend ostream& operator<<(ostream& os, const TMount& m) {
        os << m.device << " " << m.mountpoint << " ";
        for (auto f : m.flags)
            os << f << " ";

        return os;
    }

    string Mountpoint() {
        return mountpoint;
    }

    set<string> Flags() {
        return flags;
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

    set<TMount*>& Mounts() {
        return mounts;
    }

    friend ostream& operator<<(ostream& os, const TMountState& ms) {
        for (auto m : ms.mounts)
            os << *m << endl;

        return os;
    }
};
