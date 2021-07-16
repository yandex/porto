import porto
from test_common import *

conn = porto.Connection()

v = conn.CreateVolume(layers=['ubuntu-xenial'])

def CheckUserNs(userns=True, **kwargs):
    virt_mode = kwargs.get('virt_mode', 'app')
    os_mode = virt_mode == 'os'
    devices = '/dev/fuse rw' if virt_mode != 'job' else ''

    a = conn.Run('a', userns=userns, user='1044', wait=0, net='L3 veth', devices=devices)
    b = conn.Run('a/b', user='1044', root=v.path)

    # check NET_ADMIN
    c = conn.Run('a/b/c', wait=5, unshare_on_exec=userns, user='1044', devices=devices, command="bash -c 'ip netns add test && ip netns list'", **kwargs)
    ExpectEq('0' if userns else '1', c['exit_code'])
    if not os_mode:
        ExpectEq('test' if userns else '', c['stdout'].strip())
    c.Destroy()

    # check SYS_ADMIN
    c = conn.Run('a/b/c', wait=5, unshare_on_exec=userns, user='1044', devices=devices, command="bash -c 'mount -t tmpfs tmpfs /tmp && df /tmp | grep -o tmpfs'", **kwargs)
    ExpectEq('0' if userns else '1', c['exit_code'])
    if not os_mode:
        ExpectEq('tmpfs' if userns else '', c['stdout'].strip())
    c.Destroy()

    # check devices ownership
    if devices:
        c = conn.Run('a/b/c', wait=5, unshare_on_exec=userns, user='1044', devices=devices, command="stat -c '%U %G' /dev/fuse", **kwargs)
        ExpectEq('0', c['exit_code'])
        if not os_mode:
            ExpectEq('root root', c['stdout'].strip())
        c.Destroy()

    b.Destroy()
    a.Destroy()

CheckUserNs(userns=False)
CheckUserNs(virt_mode='app')
CheckUserNs(virt_mode='job')
CheckUserNs(virt_mode='os')

conn.UnlinkVolume(v.path)
