import porto
from test_common import *
import os
import time
import shutil

conn = porto.Connection()

def ExpectRunlevel(ct, level):
    r = conn.Run(ct.name + '/runlevel', wait=10, command='bash -c \'for i in `seq 50` ; do [ "`runlevel`" = "{}" ] && break ; sleep 0.1 ; done; runlevel\''.format(level))
    ExpectEq(r['stdout'].strip(), level)
    ExpectEq(r['exit_code'], '0')
    r.Destroy()


def CheckSubCgroups(ct):
    r = conn.Run(ct.name + '/child', wait=10,
                 command='''bash -c "mkdir -p /sys/fs/cgroup/freezer/dir123/subdir567;
                            echo $$ >/sys/fs/cgroup/freezer/dir123/subdir567/cgroup.procs;
                            chmod +x /portoctl; /portoctl list"''',
                            private='portoctl shell', isolate=False)
    ExpectEq(len(r['stderr']), 0)
    r.Destroy()
    ExpectEq(conn.GetProperty("/", "porto_stat[warnings]"), "0")

def CheckCgroupHierarchy(ct, haveCgroups):
    # Check cgroup hierarchy in portoctl shell container
    r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup', private='portoctl shell', isolate=False)
    assert (len(r['stdout'].strip().split('\n')) > 1) == haveCgroups
    r.Destroy()

    # Check cgroup hierarchy in child container
    r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup')
    res_cgroups = r['stdout'].strip().split('\n')
    assert res_cgroups == ['net_cls', 'net_cls,net_prio', 'net_prio'] or res_cgroups == ['systemd']
    r.Destroy()

    if haveCgroups:
        # Create subcgroup in portoctl shell containers
        r = conn.Run(ct.name + '/child', wait=10, command='mkdir /sys/fs/cgroup/freezer/cgroup123', private='portoctl shell', isolate=False)
        ExpectEq(r['exit_code'], '0')
        assert 'cgroup123' in  os.listdir("/sys/fs/cgroup/freezer/porto/" + ct.name)
        r.Destroy()

        # Create subcgroup in child containers
        r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup/freezer', private='portoctl shell', isolate=False)
        ExpectEq(r['exit_code'], '0')
        ExpectNe(len(r['stdout']), 0)
        r.Destroy()

        unmounted_cgroups = ['net_cls', 'net_prio', 'net_cls,net_prio']

        for unmounted_cgroup in unmounted_cgroups:
            r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup/{}'.format(unmounted_cgroup), private='portoctl shell', isolate=False)
            ExpectEq(r['exit_code'], '0')
            ExpectEq(len(r['stdout']), 0)
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
    shutil.copyfile(portoctl, "{}/portoctl".format(a.GetData('root')))
    CheckSubCgroups(a)
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
