#include <iterator>
#include <sstream>

#include "cgroup.hpp"
#include "containerenv.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
}

using namespace std;

TTaskEnv::TTaskEnv(const std::string &command, const string cwd)
    : cwd(cwd) {
    // TODO: support quoting

    istringstream s(command);
    args.insert(args.end(),
                istream_iterator<string>(s),
                istream_iterator<string>());

    path = args.front();
    args.erase(args.begin());
}

string TTaskEnv::GetPath() {
    return path;
}

vector<string> TTaskEnv::GetArgs() {
    return args;
}

string TTaskEnv::GetCwd() {
    return cwd;
}

void TContainerEnv::Create() {
    for (auto cg : cgroups)
        cg->Create();
}

TError TContainerEnv::Attach() {
    pid_t self = getpid();

    for (auto cg : cgroups) {
        auto error = cg->Attach(self);
        if (!error.Ok())
            return error;
    }

    return TError();
}

