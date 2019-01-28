#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

char *fname = "./test.mapped";

int main(int argc, char **argv) {
    unsigned long anon = 0lu, file = 0lu;
    unsigned char *anon_ptr = NULL;
    unsigned char *file_ptr = NULL;
    int to_wait = 0;

    if (argc < 3)
        return 1;

    if (sscanf(argv[1], "%lu", &anon) != 1 ||
        sscanf(argv[2], "%lu", &file) != 1)
        return 1;

    if (argc > 3)
        fname = argv[3];

    if (argc > 4)
        to_wait = !strcmp("wait", argv[4]);

    anon = anon % 4096 ? anon + (4096 - anon % 4096) : anon;
    file = file % 4096 ? file + (4096 - file % 4096) : file;

    if (anon) {
        anon_ptr = (unsigned char *)mmap(NULL, anon, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

        if (anon_ptr == NULL)
            return 1;

        for (unsigned long i = 0lu; i < anon / 4096; i++)
            anon_ptr[i * 4096 + i % 4096] = i % 256;
    }

    if (file) {
        int fd = open(fname, O_CREAT | O_TRUNC | O_RDWR, S_IWUSR | S_IRUSR);
        if (fd < 0)
            return 1;

        if (unlink(fname))
            return 1;

        if (ftruncate(fd, file))
            return 1;

        file_ptr = (unsigned char *)mmap(NULL, file, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_LOCKED, fd, 0);
        if (file_ptr == NULL)
            return 1;

        if (mlock(file_ptr, file) < 0) {
            return errno == EAGAIN ? 2 : 1;
        }
    }

    if (to_wait)
        pause();

    return (anon ? munmap(anon_ptr, anon) : 0) + (file ? munmap(file_ptr, file) : 0);
}
