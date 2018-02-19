#!/usr/bin/python

import porto
from test_common import *

c = porto.Connection(timeout=10)

def CheckBaseMounts(mnt):
    Expect("rw" in mnt['/']['flag'])
    Expect("rw" in mnt['/dev']['flag'])
    Expect("rw" in mnt['/dev/pts']['flag'])
    Expect("rw" in mnt['/dev/shm']['flag'])
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

w = c.Create("w", weak=True)

root_volume = c.CreateVolume(backend='overlay', layers=['ubuntu-precise'], containers='w')
test_volume = c.CreateVolume(backend='plain', containers='w')
ro_volume = c.CreateVolume(backend='plain', read_only='true', containers='w')

# no chroot
a = c.Run("a")
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt)
a.Destroy()

# simple chroot
a = c.Run("a", root=root_volume.path)
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt)
a.Destroy()

# container binds
a = c.Run("a", root=root_volume.path, bind="{0} /test; {0} /test_ro ro; {1} /ro_vol".format(test_volume.path, ro_volume.path))
mnt = ParseMountinfo(a['root_pid'])
CheckBaseMounts(mnt)
Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])
a.Destroy()

# volumes in chroot
a = c.Run("a", root=root_volume.path, bind="{} /bin/portoctl ro".format(portoctl))
a1 = c.Run("a/a1", command="portoctl vcreate /test backend=plain")
a1.Wait()
a2 = c.Run("a/a2", command="portoctl vcreate /test_ro backend=plain read_only=true")
a2.Wait()

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
a = c.Create("a", weak=True)
a['root'] = root=root_volume.path
test_volume.Link(a, target="/test_ro", read_only=True)
ro_volume.Link(a, target="/ro_vol")

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' not in mnt)
Expect(root_volume.path + '/ro_vol' not in mnt)

# mount at start
a.Start()

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' in mnt)
Expect('ro' in mnt[root_volume.path + '/test_ro']['flag'])
Expect(root_volume.path + '/ro_vol' in mnt)
Expect('ro' in mnt[root_volume.path + '/ro_vol']['flag'])

mnt = ParseMountinfo(a['root_pid'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])

# umount at stop
a.Stop()

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' not in mnt)
Expect(root_volume.path + '/ro_vol' not in mnt)

a.Start()

mnt = ParseMountinfo(a['root_pid'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])

mnt = ParseMountinfo()
Expect(root_volume.path + '/test_ro' in mnt)
Expect('ro' in mnt[root_volume.path + '/test_ro']['flag'])
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

a.Destroy()
w.Destroy()
