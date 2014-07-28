#include <iostream>
#include <string>
#include <mutex>
#include <map>
#include <vector>

using namespace std;

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
	}

	~TContainer() {
		lock_guard<mutex> guard(lock);
		state = Destroying;

		//TBD: perform actual work
	}

	string Name() {
		return name;
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

class TContainerHolder {
	mutex lock;
	map <string, TContainer*> containers;

public:
	TContainer* Create(string name) {
		lock_guard<mutex> guard(lock);

		if (containers[name] == nullptr)
			containers[name] = new TContainer(name);
		else
			throw "container " + name + " already exists";
	}

	TContainer* Find(string name) {
		lock_guard<mutex> guard(lock);

		return containers[name];
	}

	void Destroy(string name) {
		lock_guard<mutex> guard(lock);
		delete containers[name];
		containers[name] = nullptr;
	}

	vector<string> List() {
		vector<string> ret;

		lock_guard<mutex> guard(lock);
		for (auto c : containers)
			ret.push_back(c.second->Name());

		return ret;
	}
};

int main(int argc, const char *argv[])
{
	//TMountState ms;

	return EXIT_SUCCESS;
}
