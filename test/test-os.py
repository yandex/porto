import porto
from test_common import *
import os
import time

conn = porto.Connection()

def ExpectRunlevel(ct, level):
    r = conn.Run(ct.name + '/runlevel', wait=10, command='bash -c \'for i in `seq 50` ; do [ "`runlevel`" = "{}" ] && break ; sleep 0.1 ; done; runlevel\''.format(level))
    ExpectEq(r['stdout'].strip(), level)
    ExpectEq(r['exit_code'], '0')
    r.Destroy()


def CheckCgroupHierarchy(ct, haveCgroups):
    # Check cgroup hierarchy in portoctl shell container
    r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup', private='portoctl shell', isolate=False)
    assert (len(r['stdout'].strip().split('\n')) > 1) == haveCgroups
    r.Destroy()

    # Check cgroup hierarchy in child container
    r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup')
    assert (len(r['stdout'].strip().split('\n')) <= 1)
    r.Destroy()

    if haveCgroups:
        # Create subcgroup in portoctl shell containers
        r = conn.Run(ct.name + '/child', wait=10, command='mkdir /sys/fs/cgroup/freezer/cgroup123', private='portoctl shell', isolate=False)
        ExpectEq(r['exit_code'], '0')
        assert 'cgroup123' in  os.listdir("/sys/fs/cgroup/freezer/porto/" + ct.name)
        r.Destroy()

        # Create subcgroup in child containers
        r = conn.Run(ct.name + '/child', wait=10, command='mkdir -p /sys/fs/cgroup/freezer/cgroup123')
        ExpectNe(r['exit_code'], '0')
        r.Destroy()


try:
    ConfigurePortod('test-os', """
    container {
        use_os_mode_cgroupns : true
    }""")

    a = conn.Run("a", virt_mode='os', root_volume={'layers': ["ubuntu-precise"]})
    ExpectRunlevel(a, 'N 2')
    a.Stop()
    a.Start()
    ExpectRunlevel(a, 'N 2')
    a.Destroy()

    a = conn.Run("a", virt_mode='os', root_volume={'layers': ["ubuntu-xenial"]})
    ExpectRunlevel(a, 'N 5')
    a.Stop()
    a.Start()
    ExpectRunlevel(a, 'N 5')
    CheckCgroupHierarchy(a, True)
    a.Destroy()


    m = conn.Run("m", root_volume={'layers': ["ubuntu-precise"]})
    a = conn.Run("m/a", virt_mode='os')
    ExpectRunlevel(a, 'N 2')
    a.Stop()
    a.Start()
    ExpectRunlevel(a, 'N 2')
    m.Destroy()

    ConfigurePortod('test-os', """
    container {
        use_os_mode_cgroupns : false
    }""")

    m = conn.Run("m", root_volume={'layers': ["ubuntu-xenial"]})
    a = conn.Run("m/a", virt_mode='os')
    ExpectRunlevel(a, 'N 5')
    a.Stop()
    a.Start()
    CheckCgroupHierarchy(a, False)
    ExpectRunlevel(a, 'N 5')
    m.Destroy()

finally:
    ConfigurePortod('test-os', "")
