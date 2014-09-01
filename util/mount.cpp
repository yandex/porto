#include <sstream>

#include "util/mount.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <sys/mount.h>
#include <stdio.h>
#include <mntent.h>
}

using namespace std;

TError TMount::Mount(bool rdonly, bool bind, bool remount) const {
    TLogger::Log("mount " + Mountpoint);
    int mountflags = (rdonly ? MS_RDONLY : 0) |
                    (bind ? MS_BIND : 0) |
                    (remount ? MS_REMOUNT : 0);

    int ret = RetryBusy(10, 100, [&]{ return mount(Device.c_str(),
                                                   Mountpoint.c_str(),
                                                   Vfstype.c_str(),
                                                   mountflags,
                                                   CommaSeparatedList(Flags).c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "mount(" + Device + ", " + Mountpoint + ", " + Vfstype + ", " + to_string(mountflags) + ", " + CommaSeparatedList(Flags) + ")");

    return TError::Success();
}

TError TMount::Umount() const {
    TLogger::Log("umount " + Mountpoint);

    int ret = RetryBusy(10, 100, [&]{ return umount(Mountpoint.c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "umount(" + Mountpoint + ")");

    return TError::Success();
}

TError TMountSnapshot::Mounts(std::set<std::shared_ptr<TMount>> &mounts) const {
    FILE* f = setmntent("/proc/self/mounts", "r");
    struct mntent* m;
    while ((m = getmntent(f)) != NULL) {
           vector<string> vflags;
           set<string> flags;

           TError error = SplitString(m->mnt_opts, ',', vflags);
           if (error)
               return error;

           for (auto kv : vflags)
               flags.insert(kv);

        mounts.insert(make_shared<TMount>(m->mnt_fsname, m->mnt_dir, m->mnt_type, flags));
    }
    endmntent(f);

    return TError::Success();
}
