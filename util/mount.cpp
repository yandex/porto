#include <sstream>

#include "util/mount.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "log.hpp"

extern "C" {
#include <sys/mount.h>
#include <stdio.h>
#include <mntent.h>
}

using namespace std;

TError TMount::Mount(bool rdonly, bool bind, bool remount) {
    TLogger::Log("mount " + mountpoint);
    int mountflags = (rdonly ? MS_RDONLY : 0) |
                    (bind ? MS_BIND : 0) |
                    (remount ? MS_REMOUNT : 0);

    int ret = RetryBusy(10, 100, [&]{ return mount(device.c_str(),
                                                   mountpoint.c_str(),
                                                   vfstype.c_str(),
                                                   mountflags,
                                                   CommaSeparatedList(flags).c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "mount(" + device + ", " + mountpoint + ", " + vfstype + ", " + to_string(mountflags) + ", " + CommaSeparatedList(flags) + ")");

    return TError::Success();
}

TError TMount::Umount() {
    TLogger::Log("umount " + mountpoint);

    int ret = RetryBusy(10, 100, [&]{ return umount(mountpoint.c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "umount(" + mountpoint + ")");

    return TError::Success();
}

TError TMountSnapshot::Mounts(std::set<std::shared_ptr<TMount>> &mounts) {
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
