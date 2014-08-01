#include <iterator>
#include <sstream>

#include "cgroup.hpp"
#include "containerenv.hpp"

extern "C" {
#include <sys/types.h>
}

using namespace std;

TExecEnv::TExecEnv(const string &command) {
    // TODO: support quoting

    istringstream s(command);
    args.insert(args.end(),
                istream_iterator<string>(s),
                istream_iterator<string>());

    path = args.front();
    args.erase(args.begin());
}

std::string TExecEnv::GetPath() {
    return path;
}

std::vector<std::string> TExecEnv::GetArgs() {
    return args;
}

void TContainerEnv::Create() {
    for (auto cg : cgroups)
        cg->Create();
}

TError TContainerEnv::Enter() {
    pid_t self = getpid();

    for (auto cg : cgroups) {
        auto error = cg->Attach(self);
        if (!error.Ok())
            return error;
    }
}

