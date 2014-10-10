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

using std::string;
using std::vector;
using std::set;
using std::shared_ptr;

TError TMount::Mount(unsigned long flags) const {
    TLogger::Log() << "mount " << Target << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return mount(Source.c_str(),
                                                   Target.c_str(),
                                                   Vfstype.c_str(),
                                                   flags,
                                                   CommaSeparatedList(Flags).c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "mount(" + Source + ", " + Target + ", " + Vfstype + ", " + std::to_string(flags) + ", " + CommaSeparatedList(Flags) + ")");

    return TError::Success();
}

TError TMount::Umount() const {
    TLogger::Log() << "umount " << Target << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return umount(Target.c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "umount(" + Target + ")");

    return TError::Success();
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
            TLogger::Log() << "Can't remount " << m->GetMountpoint() << std::endl;
    }

    return TError::Success();
}
