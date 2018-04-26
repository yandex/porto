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
a.Destroy()


m = conn.Run("m", root_volume={'layers': ["ubuntu-precise"]})
a = conn.Run("m/a", virt_mode='os')
ExpectRunlevel(a, 'N 2')
a.Stop()
a.Start()
ExpectRunlevel(a, 'N 2')
m.Destroy()

m = conn.Run("m", root_volume={'layers': ["ubuntu-xenial"]})
a = conn.Run("m/a", virt_mode='os')
ExpectRunlevel(a, 'N 5')
a.Stop()
a.Start()
ExpectRunlevel(a, 'N 5')
m.Destroy()
