#include <iostream>
#include <string>
#include <mutex>
#include <map>

using namespace std;

enum cs_state {
	CS_STOPPED,
	CS_RUNNING,
	CS_PAUSED,
	CS_DESTROYING
};

class Container;
mutex containers_lock;
map<string, Container*> containers;

class Container {
	const string name;

	mutex lock;
	cs_state state;

	mutex _data_lock;
	// data

	bool CheckState(cs_state expected) {
		return state == expected;
	}

public:
	Container(const string _name) : name(_name), state(CS_STOPPED) {
		lock_guard<mutex> guard(containers_lock);

		if (containers[name] == nullptr)
			containers[name] = this;
		else
			throw "container " + name + " already exists";
	}

	~Container() {
		lock_guard<mutex> guard(lock);

		state = CS_DESTROYING;
		//TBD: perform actual work

		lock_guard<mutex> guard2(containers_lock);
		containers[name] = nullptr;
	}

	bool Start() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(CS_STOPPED))
			return false;

		state = CS_RUNNING;
		return true;
	}

	bool Stop() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(CS_RUNNING))
			return false;

		state = CS_STOPPED;
		return true;
	}

	bool Pause() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(CS_RUNNING))
			return false;

		state = CS_PAUSED;
		return true;
	}

	bool Resume() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(CS_PAUSED))
			return false;

		state = CS_RUNNING;
		return true;
	}

	string getProperty(string property);
	bool setProperty(string property, string value);

	string getData(string data);
};

int main(int argc, const char *argv[])
{
	return EXIT_SUCCESS;
}
