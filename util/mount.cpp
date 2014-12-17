#include <sstream>

#include "util/mount.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <sys/mount.h>
#include <stdio.h>
#include <mntent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <linux/loop.h>
#include <unistd.h>
}

using std::string;
using std::vector;
using std::set;
using std::shared_ptr;

TError TMount::Mount(unsigned long flags) const {
    L() << "mount " << Target.ToString() << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return mount(Source.ToString().c_str(),
                                                   Target.ToString().c_str(),
                                                   Type.c_str(),
                                                   flags,
                                                   CommaSeparatedList(Data).c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "mount(" + Source.ToString() + ", " + Target.ToString() + ", " + Type + ", " + std::to_string(flags) + ", " + CommaSeparatedList(Data) + ")");

    return TError::Success();
}

TError TMount::Umount() const {
    L() << "umount " << Target.ToString() << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return umount(Target.ToString().c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "umount(" + Target.ToString() + ")");

    return TError::Success();
}

TError TMount::Bind(bool rdonly, unsigned long flags) const {
    TError error = Mount(MS_BIND | flags);
    if (error)
        return error;

    if (!rdonly)
        return TError::Success();

    return Mount(MS_BIND | MS_REMOUNT | MS_RDONLY | flags);
}

TError TMount::BindFile(bool rdonly, unsigned long flags) const {
    TPath p(Target);
    TFile f(p);

    if (!f.Exists()) {
        TFolder d(p.DirName());
        if (!d.Exists()) {
            TError error = d.Create(0755, true);
            if (error)
                return error;
        }

        TError error = f.Touch();
        if (error)
            return error;
    }

    if (p.GetType() == EFileType::Link) {
        // TODO: ?can't mount over link
    }

    return Bind(rdonly, flags);
}

TError TMount::BindDir(bool rdonly, unsigned long flags) const {
    TPath p(Target);
    TFolder d(p);

    if (!d.Exists()) {
        TError error = d.Create(0755, true);
        if (error)
            return error;
    }

    return Bind(rdonly, flags);
}

TError TMount::MountDir(unsigned long flags) const {
    TFolder dir(Target);
    if (!dir.Exists()) {
        TError error = dir.Create();
        if (error)
            return error;
    }

    return Mount(flags);
}

TError TMountSnapshot::Mounts(std::set<std::shared_ptr<TMount>> &mounts) const {
    FILE* f = setmntent(Path.c_str(), "r");
    struct mntent* m;
    while ((m = getmntent(f)) != NULL) {
           vector<string> vflags;
           set<string> flags;

           TError error = SplitString(m->mnt_opts, ',', vflags);
           if (error)
               return error;

           for (auto kv : vflags)
               flags.insert(kv);

        mounts.insert(std::make_shared<TMount>(m->mnt_fsname, m->mnt_dir, m->mnt_type, flags));
    }
    endmntent(f);

    return TError::Success();
}

TError TMountSnapshot::RemountSlave() {
    // systemd mounts / with MS_SHARED so any changes made to it in the container
    // are propagated back (in particular, mounting new /proc breaks host)
    set<shared_ptr<TMount>> mounts;

    TError error = Mounts(mounts);
    if (error)
        return error;

    for (auto &m : mounts) {
        // we are only changing type of the mount from (possibly) MS_SHARED
        // to MS_SLAVE, nothing is remounted or mounted over
        error = m->Mount(MS_SLAVE);
        if (error)
            L() << "Can't remount " << m->GetMountpoint() << std::endl;
    }

    return TError::Success();
}

TError GetLoopDev(int &nr) {
    int cfd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (cfd < 0)
        return TError(EError::Unknown, errno, "open(/dev/loop-control)");

    nr = ioctl(cfd, LOOP_CTL_GET_FREE);
    if (nr < 0) {
        close(cfd);
        return TError(EError::Unknown, errno, "ioctl(LOOP_CTL_GET_FREE)");
    }
    close(cfd);

    return TError::Success();
}

TError PutLoopDev(const int nr) {
    TScopedFd cfd, dfd;
    TError error = TError::Success();

    if (nr < 0)
        return TError::Success();

    std::string dev = "/dev/loop" + std::to_string(nr);

    cfd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (cfd.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(/dev/loop-control)");

    int ret = ioctl(cfd.GetFd(), LOOP_CTL_REMOVE, nr);
    if (ret < 0)
        error = TError(EError::Unknown, errno, "ioctl(/dev/loop-control, LOOP_CTL_GET_FREE)");

    dfd = open(dev.c_str(), O_RDWR);
    if (dfd.GetFd() >= 0)
        error = TError(EError::Unknown, errno, "open(" + dev + ")");

    ret = ioctl(dfd.GetFd(), LOOP_CLR_FD, 0);
    if (ret < 0)
        error = TError(EError::Unknown, errno, "ioctl(" + std::to_string(dfd.GetFd()) + ", LOOP_CLR_FD)");

    return TError::Success();
}

TError TLoopMount::Mount() {
    std::string dev = "/dev/loop" + std::to_string(LoopNr);

    TScopedFd ffd, dfd;
    ffd = open(Source.ToString().c_str(), O_RDWR | O_CLOEXEC);
    if (ffd.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + Source.ToString() + ")");

    dfd = open(dev.c_str(), O_RDWR);
    if (dfd.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + dev + ")");

    if (ioctl(dfd.GetFd(), LOOP_SET_FD, ffd.GetFd()) < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_SET_FD)");

    TMount m(dev, Target, Type, {});
    return m.Mount();
}

TError TLoopMount::Umount() {
    std::string dev = "/dev/loop" + std::to_string(LoopNr);

    TMount m(dev, Target, Type, {});
    TError error = m.Umount();
    if (error)
        return error;

    TScopedFd fd;
    fd = open(dev.c_str(), O_RDWR);
    if (ioctl(fd.GetFd(), LOOP_CLR_FD, 0) < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_CLR_FD)");

    TFolder f(Target);
    return f.Remove(true);
}
