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


def Run(name, weak=True, start=True, **kwargs):
    global c
    if weak:
        ct = c.CreateWeakContainer(name)
    else:
        ct = c.Create(name)
    for property, value in kwargs.iteritems():
        ct.SetProperty(property, value)
    if start:
        ct.Start()
    if not weak:
        ct.SetProperty('weak', False)
    return ct

w = Run("w", start=False)

root_volume = c.CreateVolume(backend='overlay', layers=['ubuntu-precise'], containers='w')
test_volume = c.CreateVolume(backend='plain', containers='w')
ro_volume = c.CreateVolume(backend='plain', read_only='true', containers='w')

os.mkdir(test_volume.path + "/sub")
sub_volume = c.CreateVolume(path=test_volume.path + "/sub", backend='plain', containers='w')

# no chroot
a = Run("a")
mnt = ParseMountinfo(a.GetProperty('root_pid'))
CheckBaseMounts(mnt)
a.Destroy()

# simple chroot
a = Run("a", root=root_volume.path)
mnt = ParseMountinfo(a.GetProperty('root_pid'))
CheckBaseMounts(mnt)
a.Destroy()

# container binds
a = Run("a", root=root_volume.path, bind="{0} /test; {0} /test_ro ro rec; {1} /ro_vol".format(test_volume.path, ro_volume.path))
mnt = ParseMountinfo(a.GetProperty('root_pid'))
CheckBaseMounts(mnt)
Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('/test/sub' not in mnt)
Expect('/test_ro/sub' in mnt)
Expect('ro' in mnt['/test_ro/sub']['flag'])
Expect('/ro_vol' in mnt)
Expect('ro' in mnt['/ro_vol']['flag'])
a.Destroy()

#backend bind and rbind
a = Run("a", root=root_volume.path)
test_bind = c.CreateVolume(path=root_volume.path + "/test", backend='bind', storage=test_volume.path, containers='a')
test_ro_rbind = c.CreateVolume(path=root_volume.path + "/test_ro", backend='rbind', read_only='true', storage=test_volume.path, containers='a')
mnt = ParseMountinfo(a.GetProperty('root_pid'))
CheckBaseMounts(mnt)
Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])
Expect('/test/sub' not in mnt)
Expect('/test_ro/sub' in mnt)
Expect('ro' in mnt['/test_ro/sub']['flag'])
a.Destroy()

# volumes in chroot
a = Run("a", root=root_volume.path, bind="{} /bin/portoctl ro".format(portoctl))
a1 = Run("a/a1", command="portoctl vcreate /test backend=plain")
a1.Wait()
a2 = Run("a/a2", command="portoctl vcreate /test_ro backend=plain read_only=true")
a2.Wait()

mnt = ParseMountinfo()
Expect(root_volume.path + '/test' in mnt)
Expect('rw' in mnt[root_volume.path + '/test']['flag'])
Expect(root_volume.path + '/test_ro' in mnt)
Expect('ro' in mnt[root_volume.path + '/test_ro']['flag'])

mnt = ParseMountinfo(a.GetProperty('root_pid'))
Expect('/test' in mnt)
Expect('rw' in mnt['/test']['flag'])
Expect('/test_ro' in mnt)
Expect('ro' in mnt['/test_ro']['flag'])

a.Destroy()
w.Destroy()
