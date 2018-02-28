#!/usr/bin/python

import os
import porto
from test_common import *

AsAlice()

c = porto.Connection(timeout=10)

a = c.Run("a", symlink="tmp: /tmp; a/b: c/d")
ExpectEq(a['symlink'], "a/b: c/d; tmp: /tmp; ")
ExpectEq(a['symlink[tmp]'], "/tmp")
ExpectEq(os.readlink(a['cwd'] + "/tmp"), "../../../tmp")
ExpectEq(os.readlink(a['cwd'] + "/a/b"), "../c/d")
ExpectEq(os.stat(a['cwd'] + "/a").st_uid, alice_uid)
ExpectEq(os.stat(a['cwd'] + "/a").st_gid, alice_gid)
ExpectEq(os.stat(a['cwd'] + "/a").st_mode & 0777, 0775)
ExpectEq(os.lstat(a['cwd'] + "/a/b").st_uid, alice_uid)

v = c.CreateVolume(layers=["ubuntu-precise"], containers="a")
a.Stop()
a['symlink'] = "/foo/bar//../bar/baz: /xxx"
a['symlink[a]'] = 'b'
a['root'] = v.path
a.Start()
ExpectEq(a['symlink'], "/foo/bar/baz: /xxx; a: b; ")
ExpectEq(os.readlink(a['root'] + "/a"), "b")
ExpectEq(os.readlink(a['root'] + "/foo/bar/baz"), "../../xxx")
a.Destroy()
