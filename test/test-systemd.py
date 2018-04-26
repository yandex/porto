#!/usr/bin/python

import porto
from test_common import *

c = porto.Connection(timeout=10)

a = c.Run("a", virt_mode='os', root_volume={'layers': ["ubuntu-precise"]}, **{"controllers[systemd]": True})

mnt = ParseMountinfo(a['root_pid'])

Expect("/sys/fs/cgroup" in mnt)
Expect('ro' in mnt["/sys/fs/cgroup"]['flag'])

Expect("/sys/fs/cgroup/systemd" in mnt)
Expect('ro' in mnt["/sys/fs/cgroup/systemd"]['flag'])

Expect("/sys/fs/cgroup/systemd/porto%a" in mnt)
Expect('rw' in mnt["/sys/fs/cgroup/systemd/porto%a"]['flag'])

a.Destroy()
