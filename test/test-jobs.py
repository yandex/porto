import porto
from test_common import *

c = porto.Connection(timeout=30)

a = c.Run('a', weak=False, command='sleep 1000')
ExpectEq(a['state'], 'running')
b = c.Run('a/b', weak=False, virt_mode='job', command='sleep 1000')
ExpectEq(b['state'], 'running')
ExpectEq(a['cgroups'], b['cgroups'])
ReloadPortod()
ExpectEq(b['state'], 'running')
b.Kill(9)
b.Wait()
ExpectEq(b['state'], 'dead')
ExpectEq(b['exit_code'], '-9')
b.Destroy()
ExpectEq(a['state'], 'running')
a.Destroy()
