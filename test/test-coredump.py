import os
import time
import porto
import subprocess
from test_common import *

ConfigurePortod('test-coredump', """
core {
    enable: true
    default_pattern: "/tmp/core"
}
""")

if os.path.exists('/tmp/core'):
    os.unlink('/tmp/core')

if os.path.exists('/tmp/core-b'):
    os.unlink('/tmp/core-b')

conn = porto.Connection()

a = conn.Run('a', command='sleep 100', ulimit='core: 0')

a.Kill(3)
a.Wait()
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectEq(Catch(a.GetProperty, 'CORE.dumped'), porto.exceptions.LabelNotFound)
Expect(not os.path.exists('/tmp/core'))
a.Stop()


a['ulimit[core]'] = 'unlimited'
a.Start()
a.Kill(3)
a.Wait()
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '2')
ExpectProp(a, 'CORE.dumped', '1')
Expect(os.path.exists('/tmp/core'))
ExpectEq(os.stat('/tmp/core').st_uid, 0)
ExpectEq(os.stat('/tmp/core').st_gid, 0)
ExpectEq(os.stat('/tmp/core').st_mode & 0o777, 0o440)
os.unlink('/tmp/core')
a.Stop()


a['core_command'] = 'cp --sparse=always /dev/stdin crash-${CORE_EXE_NAME}-${CORE_PID}-S${CORE_SIG}.core'
a.Start()
a.Kill(3)
a.Wait()
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '3')
ExpectProp(a, 'CORE.dumped', '2')
Expect(not os.path.exists('/tmp/core'))
Expect(os.path.exists('/place/porto/a/crash-sleep-2-S3.core'))
ExpectEq(os.stat('/place/porto/a/crash-sleep-2-S3.core').st_uid, 0)
ExpectEq(os.stat('/place/porto/a/crash-sleep-2-S3.core').st_gid, 0)
ExpectEq(os.stat('/place/porto/a/crash-sleep-2-S3.core').st_mode & 0o777, 0o664)
a.Stop()

a.Destroy()


a = conn.Run("a")
b = conn.Run("a/b")
c = conn.Run('a/b/c', command='sleep 100', ulimit='core: 0')
c.Kill(3)
c.Wait()
ExpectProp(c, 'exit_code', '-3')
ExpectProp(c, 'core_dumped', True)
ExpectProp(c, 'CORE.total', '1')
ExpectProp(b, 'CORE.total', '1')
ExpectProp(a, 'CORE.total', '1')
ExpectEq(Catch(a.GetProperty, 'CORE.dumped'), porto.exceptions.LabelNotFound)

a.Destroy()


Expect(not os.path.exists('/tmp/core'))


open('/tmp/suid_sleep', 'w').write(open('/bin/sleep').read())
os.chmod('/tmp/suid_sleep', 0o6755)


open('/proc/sys/fs/suid_dumpable', 'w').write('0')


AsAlice()
conn = porto.Connection()


a = conn.Run('a', command='/tmp/suid_sleep 100', ulimit='core: unlimited')
a.Kill(3)
a.Wait()
Expect(not os.path.exists('/tmp/core'))
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', False)
ExpectEq(Catch(a.GetProperty, 'CORE.total'), porto.exceptions.LabelNotFound)
ExpectEq(Catch(a.GetProperty, 'CORE.dumped'), porto.exceptions.LabelNotFound)


b = conn.Run('b', command='/tmp/suid_sleep 100', ulimit='core: unlimited', core_command='cp --sparse=always /dev/stdin /tmp/core-b')
b.Kill(3)
b.Wait()
Expect(not os.path.exists('/tmp/core-b'))
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', False)
ExpectEq(Catch(a.GetProperty, 'CORE.total'), porto.exceptions.LabelNotFound)
ExpectEq(Catch(a.GetProperty, 'CORE.dumped'), porto.exceptions.LabelNotFound)


AsRoot()
open('/proc/sys/fs/suid_dumpable', 'w').write('1')
AsAlice()


a.Stop()
a.Start()
a.Kill(3)
a.Wait()
Expect(os.path.exists('/tmp/core'))
ExpectEq(os.stat('/tmp/core').st_uid, alice_uid)
ExpectEq(os.stat('/tmp/core').st_gid, alice_gid)
ExpectEq(os.stat('/tmp/core').st_mode & 0o777, 0o440)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
os.unlink('/tmp/core')


b.Stop()
b.Start()
b.Kill(3)
b.Wait()
Expect(os.path.exists('/tmp/core-b'))
ExpectEq(os.stat('/tmp/core-b').st_uid, alice_uid)
ExpectEq(os.stat('/tmp/core-b').st_gid, alice_gid)
ExpectEq(os.stat('/tmp/core-b').st_mode & 0o777, 0o664)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
os.unlink('/tmp/core-b')


AsRoot()
open('/proc/sys/fs/suid_dumpable', 'w').write('2')
AsAlice();


a.Stop()
a.Start()
a.Kill(3)
a.Wait()
Expect(os.path.exists('/tmp/core'))
ExpectEq(os.stat('/tmp/core').st_uid, 0)
ExpectEq(os.stat('/tmp/core').st_gid, 0)
ExpectEq(os.stat('/tmp/core').st_mode & 0o777, 0o440)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '2')
ExpectProp(a, 'CORE.dumped', '2')


b.Stop()
b.Start()
b.Kill(3)
b.Wait()
Expect(os.path.exists('/tmp/core'))
ExpectEq(os.stat('/tmp/core').st_uid, 0)
ExpectEq(os.stat('/tmp/core').st_gid, 0)
ExpectEq(os.stat('/tmp/core').st_mode & 0o777, 0o440)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '2')
ExpectProp(a, 'CORE.dumped', '2')

a.Destroy();
b.Destroy();


w = conn.Create('w')
v = conn.CreateVolume(layers=['ubuntu-xenial'], containers='w')
AsRoot()
os.unlink('/tmp/core')
open(v.path + '/bin/suid_sleep', 'w').write(open(v.path + '/bin/sleep').read())
os.chmod(v.path + '/bin/suid_sleep', 0o6755)
AsAlice()


a = conn.Run('a', root=v.path, command='sleep 100', ulimit='core: unlimited')
a.Kill(3)
a.Wait()
Expect(os.path.exists('/tmp/core'))
ExpectEq(os.stat('/tmp/core').st_uid, alice_uid)
ExpectEq(os.stat('/tmp/core').st_gid, alice_gid)
ExpectEq(os.stat('/tmp/core').st_mode & 0o777, 0o440)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
os.unlink('/tmp/core')
a.Destroy()


a = conn.Run('a', root=v.path, command='suid_sleep 100', ulimit='core: unlimited')
a.Kill(3)
a.Wait()
Expect(os.path.exists('/tmp/core'))
ExpectEq(os.stat('/tmp/core').st_uid, 0)
ExpectEq(os.stat('/tmp/core').st_gid, 0)
ExpectEq(os.stat('/tmp/core').st_mode & 0o777, 0o440)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
a.Destroy()


AsRoot()
os.unlink('/tmp/core')
AsAlice()


a = conn.Run('a', root=v.path, command='sleep 100', ulimit='core: unlimited', core_command='cp --sparse=always /dev/stdin core')
a.Kill(3)
a.Wait()
Expect(os.path.exists(v.path + '/core'))
ExpectEq(os.stat(v.path + '/core').st_uid, alice_uid)
ExpectEq(os.stat(v.path + '/core').st_gid, alice_gid)
ExpectEq(os.stat(v.path + '/core').st_mode & 0o777, 0o664)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
os.unlink(v.path + '/core')
a.Destroy()


a = conn.Run('a', root=v.path, command='suid_sleep 100', ulimit='core: unlimited', core_command='cp --sparse=always /dev/stdin core')
a.Kill(3)
a.Wait()
Expect(os.path.exists(v.path + '/core'))
ExpectEq(os.stat(v.path + '/core').st_uid, alice_uid)
ExpectEq(os.stat(v.path + '/core').st_gid, alice_gid)
ExpectEq(os.stat(v.path + '/core').st_mode & 0o777, 0o664)
ExpectProp(a, 'exit_code', '-3')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
os.unlink(v.path + '/core')
a.Destroy()

# core in container with cgroupfs=rw and core is child cgroup
AsRoot()

ConfigurePortod('test-coredump', """
container {
    enable_rw_cgroupfs: true
},
core {
    enable: true
    default_pattern: "/tmp/core"
},
""")

AsAlice()

command="bash -c 'mkdir /sys/fs/cgroup/freezer/test && echo $$ | tee /sys/fs/cgroup/freezer/test/cgroup.procs && suid_sleep 100'"

a = conn.Run('a', root=v.path, command=command, ulimit='core: unlimited', cgroupfs='rw', core_command='cp --sparse=always /dev/stdin core')
time.sleep(3)
a.Kill(6)
a.Wait()
Expect(os.path.exists(v.path + '/core'))
ExpectEq(os.stat(v.path + '/core').st_uid, alice_uid)
ExpectEq(os.stat(v.path + '/core').st_gid, alice_gid)
ExpectEq(os.stat(v.path + '/core').st_mode & 0o777, 0o664)
ExpectProp(a, 'exit_code', '-6')
ExpectProp(a, 'core_dumped', True)
ExpectProp(a, 'CORE.total', '1')
ExpectProp(a, 'CORE.dumped', '1')
os.unlink(v.path + '/core')
a.Destroy()

v.Destroy()
w.Destroy()


AsRoot()
subprocess.call(['find', '/tmp', '-maxdepth', '1', '-name', '*.core', '-delete'])
#os.unlink('/tmp/core')
os.unlink('/tmp/suid_sleep')
ConfigurePortod('test-coredump', '')
