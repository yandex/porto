#include <string>
#include <csignal>

#include "stat.hpp"
#include "util/log.hpp"

extern "C" {
#include <semaphore.h>
#include <fcntl.h>
}

void StatInc(const std::string &name) {
    sem_t *sem = sem_open(name.c_str(), O_CREAT, 0755, 0);
    if (!sem) {
        TError error(EError::Unknown, errno, "sem_open(" + name + ")");
        TLogger::LogError(error, "Can't increase statistics");
        return;
    }

    if (sem_post(sem) < 0) {
        if (errno == EOVERFLOW) {
            sem_close(sem);
            sem_unlink(name.c_str());
            return;
        }
    }
    sem_close(sem);
}

void StatReset(const std::string &name) {
    sem_unlink(name.c_str());
}

int StatGet(const std::string &name) {
    sem_t *sem = sem_open(name.c_str(), 0);
    if (!sem)
        return 0;

    int val = 0;
    sem_getvalue(sem, &val);
    sem_close(sem);
    return val;
}
