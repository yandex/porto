#include <vector>
#include <string>

extern "C" {
    #include <stdint.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <string.h>
    #include <stdio.h>
    #include <alloca.h>
}

std::vector<std::pair<void *, uint64_t> > mappings;
std::vector<int> fds;
constexpr const char *fname_fmt = "file%lu-%d.mapped";

int inline check_page_filled(unsigned char *ptr, unsigned char val) {
    int i, ret = 0;

    for (i = 0; i < 4096 && !ret; i++)
        ret = ptr[i] != val;

    return ret;
}

int anon(unsigned long size, bool shared = false) {
    if (size % 4096)
        size += (4096 - size % 4096);

    unsigned char *ptr = (unsigned char *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                               (shared ? MAP_SHARED : MAP_PRIVATE) |
                                               MAP_ANONYMOUS, 0, 0);

    if (int64_t(ptr) < 0)
        return 1;

    mappings.emplace_back(ptr, size);

    return 0;
}

char fname[4096];

int file(unsigned long size) {
    int pid;

    if (size % 4096)
        size += (4096 - size % 4096);

    pid = getpid();

    int length = snprintf(NULL, 0, fname_fmt, fds.size(), pid);

    if (!length)
        return 1;

    if (!sprintf(fname, fname_fmt, fds.size(), pid))
        return 1;

    int fd = open(fname, O_CREAT | O_TRUNC | O_RDWR, S_IWUSR | S_IRUSR);
    if (fd < 0)
        return 1;

    fds.emplace_back(fd);

    if (unlink(fname))
        return 1;

    if (ftruncate(fd, size) < 0)
        return 1;

    unsigned char *ptr = (unsigned char *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                               MAP_SHARED | MAP_LOCKED, fd, 0);
    if (int64_t(ptr) < 0)
        return 1;

    mappings.emplace_back(ptr, size);

    if (mlock(ptr, size))
        return 1;

    return 0;
}

int access(int index, bool do_read = true, bool do_msync = false) {
    unsigned char *ptr = (unsigned char *)mappings[index].first;
    unsigned long size = mappings[index].second;
    unsigned long pages = size / 4096;

    for (unsigned long j = 0lu; j < pages; j++)
        memset(ptr + j * 4096, j % 256, 4096);

    if (do_msync)
        if (msync(ptr, size, MS_ASYNC))
            return 1;

    if (do_read) {
        for (unsigned long j = 0lu; j < pages; j++)
            if (memcmp(ptr + j * 4096, ptr + j * 4096 + 2048, 2048))
                return 1;
    }

    return 0;
}

int access_fork(int index, bool do_msync = false) {
    unsigned char *ptr = (unsigned char *)mappings[index].first;
    unsigned long size = mappings[index].second;
    unsigned long pages = size / 4096;
    int ret;

    memset(ptr, 0x42, size);

    if (do_msync)
        if (msync(ptr, size, MS_ASYNC))
            return 1;

    int pid = fork();
    if (pid < 0) {
        return 1;

    } else if (!pid) {
        if (pages) {
            if (check_page_filled(ptr, 0x42))
                _exit(1);

            for (unsigned long i = 1lu; i < pages; i++)
                if (memcmp(ptr + i * 4096, ptr, 4096))
                    _exit(1);

            memset(ptr, 0xeb, size);

            if (do_msync)
                if (msync(ptr, size, MS_ASYNC))
                    _exit(1);
        }

        _exit(0);
    }

    if (wait(&ret) < 0)
        return 1;

    ret = WEXITSTATUS(ret);
    if (ret)
        return 1;

    if (pages) {
        if (check_page_filled(ptr, 0xeb))
            return 1;

        for (unsigned long i = 1lu; i < pages; i++)
            if (memcmp(ptr + i * 4096, ptr, 4096))
                return 1;
    }

    return 0;
}

int cleanup_context(void) {
    int ret = 0;

    for (auto &m : mappings)
        ret |= munmap(m.first, m.second);

    for (auto &f : fds)
        ret |= close(f);

    return ret;
}

int main(int argc, char **argv) {
    int ret = 0, arg = 1;

    if (argc < 2)
        return 1;

    while (arg < argc && !ret) {
        std::string command(argv[arg++]);
        unsigned long cmd_arg = std::stoul(argv[arg++]);

        if (command == "anon")
            ret |= anon(cmd_arg);

        else if (command == "file")
            ret |= file(cmd_arg);

        else if (command == "access")
            ret |= access(cmd_arg);

        else if (command == "access_fork")
            ret |= access_fork(cmd_arg);

        else if (command == "sleep")
            ret |= sleep(cmd_arg);

        else
            ret = 1;
    }

    if (ret)
        printf("%s\n", strerror(errno));

    cleanup_context();

    return ret;
}
