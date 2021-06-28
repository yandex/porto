import porto
import subprocess
from test_common import *

conn = porto.Connection()

try:
    ConfigurePortod('test-docker', """
    container {
         use_os_mode_cgroupns : true,
         enable_docker_mode: true
    }""")

    a = conn.Run('a', virt_mode='os', net='inherited', root_volume={'layers': ['docker-xenial', 'ubuntu-xenial']})

    b = conn.Run('a/b', wait=3, virt_mode='docker', user='porto-alice', group='porto-alice', command='grep Cap /proc/self/status')
    
    ExpectEq(b['exit_code'], '0')
    ExpectNe(b['stdout'].count('0000003fffffffff'), 0)
    ExpectEq(len(b['stderr']), 0)

    b.Destroy()

    b = conn.Run('a/b', wait=3, virt_mode='docker', user='porto-alice', group='porto-alice', command='bash -c "echo lala > /proc/sys/kernel/core_pattern"')
    ExpectNe(b['exit_code'], '0')
    ExpectEq(b['stderr'].count('Permission denied'), 1)
    b.Destroy()

    uid = subprocess.check_output(['id', 'porto-alice', '--user']).strip()
    gid = subprocess.check_output(['id', 'porto-alice', '--group']).strip()

    # change owner
    for dir in ['/run']:
        b = conn.Run('a/b', wait=5, command='chown {}:{} {}'.format(uid, gid, dir))
        ExpectEq(b['exit_code'], '0')
        b.Destroy()

    # change owner recursive
    for dir in ['/var/lib/docker', '/var/lib/containerd', '/etc/docker', '/etc/containerd']:
        b = conn.Run('a/b', wait=10, command='chown -R {}:{} {}'.format(uid, gid, dir))
        if dir != '/sys/fs/cgroup':
            ExpectEq(b['exit_code'], '0')
        b.Destroy()

    # load modules for docker
    subprocess.check_call(['modprobe', 'ip_tables'])
    subprocess.check_call(['modprobe', 'iptable_nat'])

    # start dockerd/containerd in user namespace
    c = conn.Run('a/c', wait=0, virt_mode='docker', user='porto-alice', group='porto-alice')
    time.sleep(5)
    print(c['stderr'])

    ExpectEq(c['state'], 'running')

    # run docker containers in dockerd with user namespace
    b = conn.Run('a/c/b', wait=30, command='docker run hello-world', user='porto-alice', group='porto-alice')
    ExpectEq(b['exit_code'], '0')
    b.Destroy()

    b = conn.Run('a/c/b', wait=30, command='docker run --privileged hello-world', user='porto-alice', group='porto-alice')
    ExpectEq(b['exit_code'], '0')
    b.Destroy()


    c.Destroy()
    a.Destroy()

finally:
    ConfigurePortod('test-docker', '')
