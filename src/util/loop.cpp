#include <mutex>

#include "util/loop.hpp"
#include "util/path.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <sys/types.h>
#include <linux/kdev_t.h>
#include <linux/loop.h>
}

static std::mutex BigLoopLock;

TError SetupLoopDev(int &loopNr, const TPath &image, bool ro) {
    TFile ctl, img, dev;
    struct loop_info64 info;
    int nr, retry = 10;
    TError error;

    if (ro)
        error = img.OpenRead(image);
    else
        error = img.OpenReadWrite(image);
    if (error)
        return error;

    error = ctl.OpenReadWrite("/dev/loop-control");
    if (error)
        return error;

    auto lock = std::unique_lock<std::mutex>(BigLoopLock);

again:
    nr = ioctl(ctl.Fd, LOOP_CTL_GET_FREE);
    if (nr < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_CTL_GET_FREE)");

    error = dev.OpenReadWrite("/dev/loop" + std::to_string(nr));
    if (error)
        return error;

    if (ioctl(dev.Fd, LOOP_SET_FD, img.Fd) < 0) {
        if (errno == EBUSY) {
            if (!ioctl(dev.Fd, LOOP_GET_STATUS64, &info) || errno == ENXIO) {
                if (--retry > 0)
                    goto again;
            }
        }
        return TError(EError::Unknown, errno, "ioctl(LOOP_SET_FD)");
    }

    memset(&info, 0, sizeof(info));
    strncpy((char *)info.lo_file_name, image.c_str(), LO_NAME_SIZE);

    if (ioctl(dev.Fd, LOOP_SET_STATUS64, &info) < 0) {
        error = TError(EError::Unknown, errno, "ioctl(LOOP_SET_STATUS64)");
        (void)ioctl(dev.Fd, LOOP_CLR_FD, 0);
        return error;
    }

    loopNr = nr;
    return error;
}

TError PutLoopDev(const int loopNr) {
    TFile loop;
    TError error = loop.OpenReadWrite("/dev/loop" + std::to_string(loopNr));
    if (!error && ioctl(loop.Fd, LOOP_CLR_FD, 0) < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_CLR_FD)");
    return error;
}
