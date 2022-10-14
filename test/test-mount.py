#!/usr/bin/python

import porto
from test_common import *

# now allow suid binaries in linked volumes
nosuid = False

conn = porto.Connection(timeout=10)

def CheckBaseMounts(mnt, chroot=True):
    Expect('rw' in mnt['/']['flag'])
    Expect('nosuid' not in mnt['/']['flag'])
    Expect(not chroot or 'nodev' in mnt['/']['flag'])
    Expect("rw" in mnt['/dev']['flag'])
    Expect('nodev' not in mnt['/dev']['flag'])
    Expect("rw" in mnt['/dev/pts']['flag'])
    Expect('nodev' not in mnt['/dev/pts']['flag'])
    Expect(not chroot or "rw" in mnt['/dev/shm']['flag'])
    Expect("rw" in mnt['/run']['flag'])
    Expect("ro" in mnt['/sys']['flag'])
    Expect("rw" in mnt['/proc']['flag'])
    Expect("ro" in mnt['/proc/sysrq-trigger']['flag'])
    Expect("ro" in mnt['/proc/irq']['flag'])
    Expect("ro" in mnt['/proc/bus']['flag'])
    Expect("ro" in mnt['/proc/sys']['flag'])
    Expect("ro" in mnt['/proc/kcore']['flag'])
    Expect('/sys/fs/cgroup/cpu' not in mnt)
    Expect('/sys/fs/cgroup/memory' not in mnt)

w = conn.Create("w", weak=True)

root_volume = conn.CreateVolume(backend='overlay', layers=['ubuntu-precise'], containers='w')
test_volume = conn.CreateVolume(backend='plain', containers='w')
ro_volume = conn.CreateVolume(backend='plain', read_only='true', containers='w')

os.mkdir(test_volume.path + "/sub")
sub_volume = conn.CreateVolume(path=test_volume.path + "/sub", backend='plain', containers='w')

host_mnt = ParseMountinfo()

Expect('rw' in host_mnt[root_volume.path]['flag'])
Expect('nodev' in host_mnt[root_volume.path]['flag'])
Expect('nosuid' in host_mnt[root_volume.path]['flag'] if nosuid else True)

Expect('rw' in host_mnt[test_volume.path]['flag'])
Expect('nodev' in host_mnt[test_volume.path]['flag'])
Expect('nosuid' in host_mnt[test_volume.path]['flag'] if nosuid else True)

Expect('ro' in host_mnt[ro_volume.path]['flag'])
Expect('nodev' in host_mnt[ro_volume.path]['flag'])
Expect('nosuid' in host_mnt[ro_volume.path]['flag'] if nosuid else True)

# no chroot
a = conn.Run("a")
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt, chroot=False)
a.Destroy()

# simple chroot
a = conn.Run("a", root=root_volume.path)
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt)
a.Destroy()

# container binds
a = conn.Run("a", root=root_volume.path, bind="{0} /test; {0} /test_ro ro,rec; {1} /ro_vol".format(test_volume.path, ro_volume.path))
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt)
Expect('/' in mnt)
Expect('nosuid' not in mnt['/']['flag'])

Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('nodev' in mnt['/test']['flag'])
Expect('nosuid' not in mnt['/test']['flag'])
Expect('shared' in mnt['/test'])
Expect('master' in mnt['/test'])
ExpectEq(mnt['/test']['master'], host_mnt[test_volume.path]['shared'])

Expect('/test/sub' not in mnt)

Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('shared' in mnt['/test_ro'])
Expect('master' in mnt['/test_ro'])
ExpectEq(mnt['/test_ro']['master'], host_mnt[test_volume.path]['shared'])

Expect('/test_ro/sub' in mnt)
Expect('ro' in mnt['/test_ro/sub']['flag'])
Expect('shared' in mnt['/test_ro/sub'])
Expect('master' in mnt['/test_ro/sub'])
ExpectEq(mnt['/test_ro/sub']['master'], host_mnt[sub_volume.path]['shared'])

Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])

# container binds propagation
os.mkdir(test_volume.path + "/sub2")
test_sub2 = conn.CreateVolume(path=test_volume.path + "/sub2", backend='plain', containers='a')
mnt = ParseMountinfo(a['root_pid'])
Expect('/test/sub2' in mnt)
Expect('rw' in mnt['/test/sub2']['flag'])
Expect('/test_ro/sub2' in mnt)
Expect('rw' in mnt['/test_ro/sub2']['flag'])

a.Destroy()

# bind into asbolute symlink in chroot
os.mkdir(root_volume.path + "/symlink_dst")
os.symlink("/symlink_dst", root_volume.path + "/symlink_src")

a = conn.Run("a", root=root_volume.path, bind="{0} /symlink_src".format(test_volume.path))
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt)
Expect('/' in mnt)

Expect('/symlink_dst' in mnt)
Expect('master' in mnt['/symlink_dst'])
ExpectEq(mnt['/symlink_dst']['master'], host_mnt[test_volume.path]['shared'])

a.Destroy()

#backend bind and rbind
a = conn.Run("a", root=root_volume.path)
os.mkdir(root_volume.path + "/test_rbind")
os.mkdir(root_volume.path + "/test_ro_rbind")
test_bind = conn.CreateVolume(path=root_volume.path + "/test", backend='bind', storage=test_volume.path, containers='a')
test_rbind = conn.CreateVolume(path=root_volume.path + "/test_rbind", backend='rbind', storage=test_volume.path, containers='a')
test_ro_rbind = conn.CreateVolume(path=root_volume.path + "/test_ro_rbind", backend='rbind', read_only='true', storage=test_volume.path, containers='a')

mnt = ParseMountinfo()

Expect(root_volume.path + "/test" in mnt)

Expect(root_volume.path + "/test/sub" not in mnt)

Expect(root_volume.path + "/test_rbind" in mnt)
Expect('rw' in mnt[root_volume.path + "/test_rbind"]['flag'])

Expect(root_volume.path + "/test_rbind/sub" in mnt)
Expect('rw' in mnt[root_volume.path + "/test_rbind/sub"]['flag'])

Expect(root_volume.path + "/test_ro_rbind" in mnt)
Expect('ro' in mnt[root_volume.path + "/test_ro_rbind"]['flag'])

Expect(root_volume.path + "/test_ro_rbind/sub" in mnt)
Expect('ro' in mnt[root_volume.path + "/test_ro_rbind/sub"]['flag'])

mnt = ParseMountinfo(a['root_pid'])

CheckBaseMounts(mnt)

Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('master' in mnt['/test'])
Expect('shared' in mnt['/test'])

Expect('nodev' in mnt['/test']['flag'])
Expect('nosuid' in mnt['/test']['flag'] if nosuid else True)

Expect('/test/sub' not in mnt)

Expect('/test_rbind' in mnt)
Expect('rw' in mnt['/test_rbind']['flag'])

Expect('/test_rbind/sub' in mnt)
Expect('rw' in mnt['/test_rbind/sub']['flag'])

Expect('/test_ro_rbind' in mnt)
Expect('ro' in mnt['/test_ro_rbind']['flag'])
Expect('master' in mnt['/test_ro_rbind'])
Expect('shared' in mnt['/test_ro_rbind'])

Expect('/test_ro_rbind/sub' in mnt)
Expect('ro' in mnt['/test_ro_rbind/sub']['flag'])
Expect('master' in mnt['/test_ro_rbind/sub'])
Expect('shared' in mnt['/test_ro_rbind/sub'])

# backend bind and rbind propagation
test_sub2 = conn.CreateVolume(path=test_volume.path + "/sub2", backend='plain', containers='a')
mnt = ParseMountinfo(a['root_pid'])

Expect('/test/sub2' in mnt)
Expect('rw' in mnt['/test/sub2']['flag'])

Expect('/test_rbind/sub2' in mnt)
Expect('rw' in mnt['/test_rbind/sub2']['flag'])

Expect('/test_ro_rbind/sub2' in mnt)
Expect('rw' in mnt['/test_ro_rbind/sub2']['flag']) # no ro propagation

# no backward propagation
test_sub2.Link(a, target="/test_rbind/sub2_link")
mnt = ParseMountinfo()
Expect(root_volume.path + "/test_rbind/sub2_link" in mnt)
Expect(root_volume.path + "/test_ro_rbind/sub2_link" not in mnt)
Expect(test_volume.path + "/sub2_link" not in mnt)
mnt = ParseMountinfo(a['root_pid'])
Expect('/test_rbind/sub2_link' in mnt)
Expect('/test_ro_rbind/sub2_link' not in mnt)
a.Destroy()

# volumes in chroot
a = conn.Run("a", root=root_volume.path, bind="{} /bin/portoctl ro".format(portoctl))
a1 = conn.Run("a/a1", wait=60, command="portoctl vcreate /test backend=plain")
a2 = conn.Run("a/a2", wait=60, command="portoctl vcreate /test_ro backend=plain read_only=true")

mnt = ParseMountinfo()
Expect(root_volume.path + '/test' in mnt)
Expect('rw' in mnt[root_volume.path + '/test']['flag'])
Expect(root_volume.path + '/test_ro' in mnt)
Expect('ro' in mnt[root_volume.path + '/test_ro']['flag'])

mnt = ParseMountinfo(a['root_pid'])
Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])

a.Destroy()


# volume links
a = conn.Create("a", weak=True)
a['root'] = root=root_volume.path
test_volume.Link(a, target="/test_ro", read_only=True)
ro_volume.Link(a, target="/ro_vol", read_only=True)
test_volume.Link(a, target="/test")
test_volume.Link(a, target="/test/test")

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' in mnt)
Expect(root_volume.path + '/ro_vol' in mnt)
Expect(root_volume.path + '/test' in mnt)
Expect(root_volume.path + '/test/test' in mnt)

# mount at start
a.Start()

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' in mnt)
Expect('ro' in mnt[root_volume.path + '/test_ro']['flag'])

Expect('nodev' in mnt[root_volume.path + '/test_ro']['flag'])
Expect('nosuid' in mnt[root_volume.path + '/test_ro']['flag'] if nosuid else True)

Expect(root_volume.path + '/test_ro/sub' not in mnt)
Expect(root_volume.path + '/ro_vol' in mnt)
Expect('ro' in mnt[root_volume.path + '/ro_vol']['flag'])
Expect(root_volume.path + '/test' in mnt)
Expect(root_volume.path + '/test/test' in mnt)

mnt = ParseMountinfo(a['root_pid'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])

Expect('nodev' in mnt['/test_ro']['flag'])
Expect('nosuid' in mnt['/test_ro']['flag'] if nosuid else True)

Expect('/test_ro/sub' not in mnt)
Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])
Expect('/test' in mnt)
Expect('/test/test' in mnt)

# umount at stop
a.Stop()

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' in mnt)
Expect(root_volume.path + '/ro_vol' in mnt)

a.Start()

mnt = ParseMountinfo(a['root_pid'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('/test_ro/sub' not in mnt)
Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' in mnt)
Expect('ro' in mnt[root_volume.path + '/test_ro']['flag'])
Expect(root_volume.path + '/test_ro/sub' not in mnt)
Expect(root_volume.path + '/ro_vol' in mnt)
Expect('ro' in mnt[root_volume.path + '/ro_vol']['flag'])

# umount at unlink
test_volume.Unlink(a)
ro_volume.Unlink(a)

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' not in mnt)
Expect(root_volume.path + '/ro_vol' not in mnt)

mnt = ParseMountinfo(a['root_pid'])
Expect('/test_ro' not in mnt)
Expect('/ro_vol' not in mnt)

# volume link removes symlink and creates directories
os.mkdir(root_volume.path + "/symlink_dst2")
os.symlink("/symlink_dst2", root_volume.path + "/symlink_src2")
test_volume.Link(a, target="/symlink_src2/sub_dir")
mnt = ParseMountinfo()
Expect(root_volume.path + '/symlink_src2/sub_dir' in mnt)

a.Destroy()


# mount bind file
for path in ("", "/tmp/kek"):
    root_a = conn.CreateVolume(containers='w')
    os.mkdir(root_a.path + "/root")
    root_b = conn.CreateVolume(root_a.path + "/root", backend='overlay', layers=['ubuntu-precise'], containers='w')

    a = conn.Run("a", root=root_a.path, weak=True)
    b = conn.Run("a/b", root="/root", weak=True)

    with open("/tmp/kek", "w") as file:
        file.write("kek")
    with open(root_b.path + "/tmp/lol", "a") as file:
        file.write("lol")
    bind_volume = conn.CreateVolume(path, backend="bind", storage="/tmp/kek", containers='w')

    ExpectEq(subprocess.check_output(["cat", "/tmp/kek"]), b"kek")
    ExpectEq(subprocess.check_output(["cat", root_b.path + "/tmp/lol"]), b"lol")

    conn.LinkVolume(bind_volume.path, "a/b", "/tmp/kek")
    conn.LinkVolume(bind_volume.path, "a/b", "/tmp/lol")

    c = conn.Run("a/b/c", command="cat /tmp/kek", wait=5)

    ExpectEq(c['exit_code'], "0")
    ExpectEq(c['stdout'], "kek")

    ExpectEq(subprocess.check_output(["cat", "/tmp/kek"]), b"kek")
    ExpectEq(subprocess.check_output(["cat", root_b.path + "/tmp/lol"]), b"kek")
    ExpectEq(subprocess.check_output(["cat", root_b.path + "/tmp/kek"]), b"kek")

    bind_volume.Destroy()
    a.Destroy()

w.Destroy()
