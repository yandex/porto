import porto
from test_common import *
import os
import time

conn = porto.Connection()

try:
    def CheckCgroupfsNone():
        a = conn.Run('a', cgroupfs='none', wait=0, root_volume={'layers': ['ubuntu-xenial']})

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='cat /proc/self/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(8, b['stdout'].count('porto%a'))
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='ls /sys/fs/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq('', b['stdout'])
        b.Destroy()

        a.Destroy()

    def CheckCgroupfsRo():
        a = conn.Run('a', cgroupfs='ro', wait=0, root_volume={'layers': ['ubuntu-xenial']})

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='cat /proc/self/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(0, b['stdout'].count('porto%a'))
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='ls /sys/fs/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(16, len(b['stdout'].split()))
        b.Destroy()

        # check cpu cgroup symlink
        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='bash -c "ls /sys/fs/cgroup/cpu | wc -l"')
        ExpectEq('0', b['exit_code'])
        ExpectNe('0', b['stdout'].strip())
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='bash -c "mkdir /sys/fs/cgroup/freezer/test && echo $$ | tee /sys/fs/cgroup/freezer/test"')
        ExpectNe('0', b['exit_code'])
        ExpectNe(-1, b['stderr'].find('Read-only file system'))
        b.Destroy()

        a.Destroy()

    def CheckCgroupfsRw(is_os):
        if is_os:
            ExpectEq(porto.exceptions.PermissionError, Catch(conn.Run, 'a', cgroupfs='rw', wait=0, root_volume={'layers': ['ubuntu-xenial']}))

        a = conn.Run('a', cgroupfs='rw', virt_mode=('os' if is_os else 'app'), wait=0, root_volume={'layers': ['ubuntu-xenial']})

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='cat /proc/self/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(0, b['stdout'].count('porto%a'))
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='ls /sys/fs/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(16, len(b['stdout'].split()))
        b.Destroy()

        b = conn.Run('a/b', wait=0, virt_mode='job', isolate=False, command='bash -c "mkdir /sys/fs/cgroup/freezer/test && echo $$ | tee /sys/fs/cgroup/freezer/test/cgroup.procs; sleep 3"')

        time.sleep(1)
        with open('/sys/fs/cgroup/freezer/porto/a/test/cgroup.procs') as f:
            ExpectEq(2, len(f.read().strip().split())) # bash and sleep in cgroup
        b.Wait()

        ExpectEq('0', b['exit_code'])
        b.Destroy()

        a.Destroy()

    CheckCgroupfsNone()
    CheckCgroupfsRo()
    ExpectEq(porto.exceptions.PermissionError, Catch(conn.Run, 'a', cgroupfs='rw', wait=0, root_volume={'layers': ['ubuntu-xenial']}))

    ConfigurePortod('test-cgroupns', """
    container {
        use_os_mode_cgroupns : true
    }""")

    CheckCgroupfsNone()
    CheckCgroupfsRo()
    CheckCgroupfsRw(True)

    ConfigurePortod('test-cgroupns', """
    container {
        enable_rw_cgroupfs: true
    }""")

    CheckCgroupfsNone()
    CheckCgroupfsRo()
    CheckCgroupfsRw(False)

finally:
    ConfigurePortod('test-cgroupns', "")
