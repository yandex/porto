#include "device.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"

extern "C" {
#include <sys/stat.h>
#include <linux/kdev_t.h>
}

TError TDevice::CheckPath(const TPath &path) {
    if (!path.IsNormal())
        return TError(EError::InvalidValue, "Non-normalized device path: {}", path);

    if (!path.IsInside("/dev"))
        return TError(EError::InvalidValue, "Device path not in /dev: {}", path);

    return OK;
}

TError TDevice::Init(const TPath &path) {
    struct stat st;
    TError error;

    error = TDevice::CheckPath(path);
    if (error)
        return error;

    error = path.StatFollow(st);
    if (error) {
        if (error.Errno == ENOENT)
            return TError(EError::DeviceNotFound, "Device {} does not exists", Path);
        return error;
    }

    if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode))
        return TError(EError::DeviceNotFound, "Not a device node: {}", path);

    Path = path;
    PathInside = path;
    Node = st.st_rdev;
    Uid = st.st_uid;
    Gid = st.st_gid;
    Mode = st.st_mode;

    MayRead = MayWrite = MayMknod = true;
    Wildcard = Privileged = false;

    return OK;
}

TError TDevice::Parse(TTuple &opt) {
    TError error;

    if (opt.size() < 2)
        return TError(EError::InvalidValue, "Invalid device config: " +
                      MergeEscapeStrings(opt, ' '));

    error = Init(opt[0]);
    if (error)
        return error;

    MayRead = MayWrite = MayMknod = false;

    for (char c: opt[1]) {
        switch (c) {
            case 'r':
                MayRead = true;
                break;
            case 'w':
                MayWrite = true;
                break;
            case 'm':
                MayMknod = true;
                break;
            case '*':
                Wildcard = true;
                Privileged = true;
                break;
            case '-':
                break;
            default:
                return TError(EError::InvalidValue, "Invalid access: " + opt[1]);
        }
    }

    /* Initial setup is done in container */
    if ((MayRead || MayWrite) && !Wildcard)
        MayMknod = true;

    if (opt.size() > 2) {
        error = TDevice::CheckPath(opt[2]);
        if (error)
            return error;
        PathInside = opt[2];
    }

    if (opt.size() > 3) {
        unsigned mode;
        error = StringToOct(opt[3], mode);
        if (error)
            return error;
        if (mode & ~0777)
            return TError(EError::InvalidValue, "invalid device mode: " + opt[3]);
        if ((Mode & 0777) < mode)
            Privileged = true;
        Mode = mode | (Mode & ~0777);
    }

    if (opt.size() > 4) {
        uid_t uid;
        error = UserId(opt[4], uid);
        if (error)
            return error;
        if (uid != Uid)
            Privileged = true;
        Uid = uid;
    }

    if (opt.size() > 5) {
        gid_t gid;
        error = GroupId(opt[5], gid);
        if (error)
            return error;
        if (gid != Gid)
            Privileged = true;
        Gid = gid;
    }

    return OK;
}

std::string TDevice::Format() const {
    std::string perm;

    if (MayRead)
        perm += "r";
    if (MayWrite)
        perm += "w";
    if (MayMknod)
        perm += "m";
    if (Wildcard)
        perm += "*";
    if (perm == "")
        perm = "-";

    return fmt::format("{} {} {} {:#o} {} {}", Path, perm, PathInside,
                       Mode & 0777, UserName(Uid), GroupName(Gid));
}

std::string TDevice::CgroupRule(bool allow) const {
    std::string rule;

    /* cgroup cannot parse rules with empty permissions */
    if (MayRead != allow && MayWrite != allow && MayMknod != allow)
        return "";

    if (S_ISBLK(Mode))
        rule = "b ";
    else
        rule = "c ";

    rule += std::to_string(major(Node)) + ":";

    if (Wildcard)
        rule += "* ";
    else
        rule += std::to_string(minor(Node)) + " ";

    if (MayRead == allow)
        rule += "r";
    if (MayWrite == allow)
        rule += "w";
    if (MayMknod == allow)
        rule += "m";

    return rule;
}

TError TDevice::Permitted(const TCred &cred) const {
    struct stat st;

    if (Privileged && !cred.IsRootUser())
        return TError(EError::Permission, "{} cannot use device {}", cred.ToString(), Path);

    if (Wildcard)
        return OK;

    TError error = Path.StatFollow(st);
    if (error) {
        if (error.Errno == ENOENT)
            return TError(EError::DeviceNotFound, "Device {} does not exists", Path);
        return error;
    }

    if (MayRead && !TFile::Access(st, cred, TFile::R))
        return TError(EError::Permission, "{} cannot read device {}", cred.ToString(), Path);

    if (MayWrite && !TFile::Access(st, cred, TFile::W))
        return TError(EError::Permission, "{} cannot write device {}", cred.ToString(), Path);

    return OK;
}

TError TDevice::Makedev(const TPath &root) const {
    TPath path = root / PathInside;
    struct stat st;
    TError error;

    error = path.DirName().MkdirAll(0755);
    if (error)
        return error;

    if (Wildcard || !MayMknod)
        return OK;

    L_ACT("Make {} device node {} {}:{} {:#o} {}:{}",
            S_ISBLK(Mode) ? "blk" : "chr", PathInside,
            major(Node), minor(Node), Mode & 0777,
            UserName(Uid), GroupName(Gid));

    if (!path.StatFollow(st)) {
        if (st.st_mode == Mode && st.st_rdev == Node &&
                st.st_uid == Uid && st.st_gid == Gid)
            return OK;
        return TError(EError::Busy, "Different device node already exists: {}", PathInside);
    }

    error = path.Mknod(Mode, Node);
    if (error)
        return error;

    error = path.Chown(Uid, Gid);
    if (error)
        return error;

    return OK;
}

TError TDevices::Parse(const std::string &str) {
    auto config = SplitEscapedString(str, ' ', ';');
    TError error;

    for (auto &cfg: config) {
        TDevice device;
        error = device.Parse(cfg);
        if (error)
            return error;
        if (device.MayRead || device.MayWrite || !device.MayMknod)
            NeedCgroup = true;
        Devices.push_back(device);
    }

    return OK;
}

std::string TDevices::Format() const {
    std::string str;
    for (auto &device: Devices)
        str += device.Format() + "; ";
    return str;
}

TError TDevices::Permitted(const TCred &cred) const {
    TError error;

    for (auto &device: Devices) {
        error = device.Permitted(cred);
        if (error)
            return error;
    }

    return OK;
}

TError TDevices::Makedev(const TPath &root) const {
    TError error;

    for (auto &device: Devices) {
        if (device.MayRead || device.MayWrite || device.MayMknod) {
            error = device.Makedev(root);
            if (error)
                return error;
        } else if (!root.IsRoot() && !device.Wildcard) {
            L_ACT("Remove device node {}", device.PathInside);
            error = (root / device.PathInside).Unlink();
            if (error && error.Errno != ENOENT)
                return error;
        }
    }

    return OK;
}

TError TDevices::Apply(const TCgroup &cg, bool reset) const {
    TError error;

    if (reset) {
        error = cg.Set("devices.deny", "a");
        if (error)
            return error;
    }

    for (auto &device: Devices) {
        std::string rule;

        rule = device.CgroupRule(true);
        if (rule != "") {
            error = cg.Set("devices.allow", rule);
            if (error)
                return error;
        }

        rule = device.CgroupRule(false);
        if (rule != "") {
            error = cg.Set("devices.deny", rule);
            if (error)
                return error;
        }
    }

    return OK;
}

TError TDevices::InitDefault() {
    TError error;

    Devices = {
        {"/dev/null", MKDEV(1, 3)},
        {"/dev/zero", MKDEV(1, 5)},
        {"/dev/full", MKDEV(1, 7)},
        {"/dev/random", MKDEV(1, 8)},
        {"/dev/urandom", MKDEV(1, 9)},
        {"/dev/tty", MKDEV(5, 0)},
        {"/dev/console", MKDEV(1, 3)},
        {"/dev/ptmx", MKDEV(5, 2)},
        {"/dev/pts/*", MKDEV(136, 0)},
    };

    Devices[6].Path = "/dev/null";
    Devices[7].MayMknod = false;
    Devices[8].Wildcard = true;
    Devices[8].MayMknod = false;

    error = Parse(config().container().extra_devices());
    if (error)
        return error;

    return OK;
}

void TDevices::Merge(const TDevices &devices, bool overwrite, bool replace) {
    if (replace) {
        for (auto &device: Devices)
            device.MayRead = device.MayWrite = device.MayMknod = false;
    }
    for (auto &device: devices.Devices) {
        bool found = false;
        for (auto &d: Devices) {
            if (d.PathInside == device.PathInside) {
                found = true;
                if (overwrite)
                    d = device;
                break;
            }
        }
        if (!found)
            Devices.push_back(device);
    }
}
