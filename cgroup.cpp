#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>

using namespace std;

set<string> subsystems = {"cpuset", "cpu", "cpuacct", "memory", "devices",
			  "freezer", "net_cls", "net_prio", "blkio",
			  "perf_event", "hugetlb"};

class TMount {
	string device;
	string mountpoint;
	string vfstype;
	set<string> flags;

public:
	TMount(string mounts_line) {
		// from single /proc/self/mounts line, like:
		// /dev/sda1 /boot ext4 rw,seclabel,relatime,data=ordered 0 0

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

	set<string> CgroupSubsystems() {
		set<string> ss;

		set_intersection(flags.begin(), flags.end(),
				 subsystems.begin(), subsystems.end(),
				 inserter(ss, ss.begin()));
		// TODO: systemd

		return ss;
	}

	friend ostream& operator<<(ostream& os, const TMount& m) {
		os << m.device << " " << m.mountpoint << " ";
		for (auto f : m.flags)
			os << f << " ";

		return os;
	}
};

class TMountState {
	set<TMount*> mounts;
	map<string, TMount*> cgroup_mounts;

public:
	TMountState() {
		ifstream s("/proc/self/mounts");
		string line;

		while (getline(s, line)) {
			TMount *mount = new TMount(line);
			mounts.insert(mount);

			set<string> ss = mount->CgroupSubsystems();
			if (!ss.empty()) {
				for (auto s : ss)
					cgroup_mounts[s] = mount;
			}
		}
	}

	~TMountState() {
		for (auto m : mounts)
			delete m;
	}
};

class TCgroup {
	string name;
	string subsystem;
	TCgroup *parent;
};
