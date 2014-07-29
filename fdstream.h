#ifndef __FDSTREAM_H__
#define __FDSTREAM_H__

#include <iostream>
#include <ext/stdio_filebuf.h>

extern "C" {
#include <unistd.h>
}

struct FdStream {
    int fd;
    __gnu_cxx::stdio_filebuf<char> ibuf;
    __gnu_cxx::stdio_filebuf<char> obuf;
    std::istream ist;
    std::ostream ost;

    FdStream(int fd) :
        fd(fd),
        ibuf(fd, std::ios_base::in, 1),
        obuf(fd, std::ios_base::out, 1),
        ist(&ibuf),
        ost(&obuf) {
    }
    ~FdStream() {
        ibuf.close();
        obuf.close();

        close(fd);
    }
};

#endif
