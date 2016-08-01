#include <sstream>
#include <mutex>
#include <array>

#include "util/mount.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"
#include "config.hpp"

extern "C" {
#include <sys/mount.h>
#include <stdio.h>
#include <mntent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <linux/kdev_t.h>
#include <linux/loop.h>
#include <unistd.h>
}

TError SetupLoopDevice(TPath image, int &dev)
{
    static std::mutex BigLoopLock;
    int control_fd, image_fd, loop_nr, loop_fd;
    struct loop_info64 info;
    std::string loop;
    int retry = 10;
    TError error;

    image_fd = open(image.c_str(), O_RDWR | O_CLOEXEC);
    if (image_fd < 0) {
        error = TError(EError::Unknown, errno, "open(" + image.ToString() + ")");
        goto err_image;
    }

    control_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (control_fd < 0) {
        error = TError(EError::Unknown, errno, "open(/dev/loop-control)");
        goto err_control;
    }

    BigLoopLock.lock();

again:
    loop_nr = ioctl(control_fd, LOOP_CTL_GET_FREE);
    if (loop_nr < 0) {
        error = TError(EError::Unknown, errno, "ioctl(LOOP_CTL_GET_FREE)");
        goto err_get_free;
    }

    loop = "/dev/loop" + std::to_string(loop_nr);
    loop_fd = open(loop.c_str(), O_RDWR | O_CLOEXEC);
    if (loop_fd < 0) {
        error = TError(EError::Unknown, errno, "open(" + loop + ")");
        goto err_loop_open;
    }

    if (ioctl(loop_fd, LOOP_SET_FD, image_fd) < 0) {
        error = TError(EError::Unknown, errno, "ioctl(LOOP_SET_FD)");
        if (errno == EBUSY) {
            if (!ioctl(loop_fd, LOOP_GET_STATUS64, &info) || errno == ENXIO) {
                if (--retry > 0) {
                    close(loop_fd);
                    goto again;
                }
            }
        }
        goto err_set_fd;
    }

    memset(&info, 0, sizeof(info));
    strncpy((char *)info.lo_file_name, image.c_str(), LO_NAME_SIZE);

    if (ioctl(loop_fd, LOOP_SET_STATUS64, &info) < 0) {
        error = TError(EError::Unknown, errno, "ioctl(LOOP_SET_STATUS64)");
        ioctl(loop_fd, LOOP_CLR_FD, 0);
        goto err_set_status;
    }

    dev = loop_nr;
    error = TError::Success();
err_set_status:
err_set_fd:
    close(loop_fd);
err_loop_open:
err_get_free:
    BigLoopLock.unlock();
    close(control_fd);
err_control:
    close(image_fd);
err_image:
    return error;
}

TError PutLoopDev(const int nr) {
    L_ACT() << "Loop device free " << nr << std::endl;

    std::string dev = "/dev/loop" + std::to_string(nr);

    TScopedFd loopFd;
    loopFd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (loopFd.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + dev + ")");

    if (ioctl(loopFd.GetFd(), LOOP_CLR_FD, 0) < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_CLR_FD)");

    return TError::Success();
}
