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

TError SetupLoopDevice(const TPath &imagePath, int &loopNr) {
    TFile control, image, loop;
    struct loop_info64 info;
    int nr, retry = 10;
    TError error;

    error = image.OpenReadWrite(imagePath);
    if (error)
        return error;

    error = control.OpenReadWrite("/dev/loop-control");
    if (error)
        return error;

    auto lock = std::unique_lock<std::mutex>(BigLoopLock);

again:
    nr = ioctl(control.Fd, LOOP_CTL_GET_FREE);
    if (nr < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_CTL_GET_FREE)");

    error = loop.OpenReadWrite("/dev/loop" + std::to_string(nr));
    if (error)
        return error;

    if (ioctl(loop.Fd, LOOP_SET_FD, image.Fd) < 0) {
        if (errno == EBUSY) {
            if (!ioctl(loop.Fd, LOOP_GET_STATUS64, &info) || errno == ENXIO) {
                if (--retry > 0)
                    goto again;
            }
        }
        return TError(EError::Unknown, errno, "ioctl(LOOP_SET_FD)");
    }

    memset(&info, 0, sizeof(info));
    strncpy((char *)info.lo_file_name, imagePath.c_str(), LO_NAME_SIZE);

    if (ioctl(loop.Fd, LOOP_SET_STATUS64, &info) < 0) {
        error = TError(EError::Unknown, errno, "ioctl(LOOP_SET_STATUS64)");
        (void)ioctl(loop.Fd, LOOP_CLR_FD, 0);
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

TError ResizeLoopDev(int loopNr, const TPath &image, off_t current, off_t target) {
    auto path = "/dev/loop" + std::to_string(loopNr);
    auto size = std::to_string(target >> 10) + "K";
    TError error;
    TFile dev;

    if (target < current)
        return TError(EError::NotSupported, "Online shrink is not supported yet");

    error = dev.OpenReadWrite(path);
    if (error)
        return error;

    error = image.Truncate(target);
    if (error)
        return error;

    if (ioctl(dev.Fd, LOOP_SET_CAPACITY, 0) < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_SET_CAPACITY)");

    return RunCommand({"resize2fs", path, size}, "/");
}
