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
}

using std::string;
using std::vector;
using std::set;
using std::shared_ptr;

TError TMount::Mount(unsigned long flags) const {
    TLogger::Log() << "mount " << Target.ToString() << std::endl;

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
    TLogger::Log() << "umount " << Target.ToString() << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return umount(Target.ToString().c_str()); });
    if (ret)
        return TError(EError::Unknown, errno, "umount(" + Target.ToString() + ")");

    return TError::Success();
}

TError TMount::Bind(bool Rdonly) const {
    TError error = Mount(MS_BIND);
    if (error)
        return error;

    if (!Rdonly)
        return TError::Success();

    return Mount(MS_BIND | MS_REMOUNT | MS_RDONLY);
}

TError TMount::BindFile(bool Rdonly) const {
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

    return Bind(Rdonly);
}

TError TMount::BindDir(bool Rdonly) const {
    TPath p(Target);
    TFolder d(p);

    if (!d.Exists()) {
        TError error = d.Create(0755, true);
        if (error)
            return error;
    }

    return Bind(Rdonly);
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
            TLogger::Log() << "Can't remount " << m->GetMountpoint() << std::endl;
    }

    return TError::Success();
}
