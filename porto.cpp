#include <iostream>
#include <string>
#include <mutex>
#include <map>

using namespace std;

class TContainer;
mutex containers_lock;
map<string, TContainer*> containers;

class TContainer {
	const string name;

	mutex lock;
	enum EContainerState {
		Stopped,
		Running,
		Paused,
		Destroying
	};
	EContainerState state;

	mutex _data_lock;
	// data

	bool CheckState(EContainerState expected) {
		return state == expected;
	}

public:
	TContainer(const string _name) : name(_name), state(Stopped) {
		lock_guard<mutex> guard(containers_lock);

		if (containers[name] == nullptr)
			containers[name] = this;
		else
			throw "container " + name + " already exists";
	}

	~TContainer() {
		lock_guard<mutex> guard(lock);
		state = Destroying;

		lock_guard<mutex> guard2(containers_lock);
		containers[name] = nullptr;

		//TBD: perform actual work
	}

	bool Start() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(Stopped))
			return false;

		state = Running;
		return true;
	}

	bool Stop() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(Running))
			return false;

		state = Stopped;
		return true;
	}

	bool Pause() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(Running))
			return false;

		state = Paused;
		return true;
	}

	bool Resume() {
		lock_guard<mutex> guard(lock);

		if (!CheckState(Paused))
			return false;

		state = Running;
		return true;
	}

	string GetProperty(string property);
	bool SetProperty(string property, string value);

	string GetData(string data);
};

int main(int argc, const char *argv[])
{
	//TMountState ms;

	return EXIT_SUCCESS;
}
