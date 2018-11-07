import porto
from test_common import *

c = porto.Connection(timeout=30)

a = c.Run('test-a', weak=False, command='sleep 1000')
ExpectEq(a['state'], 'running')

ExpectEq(Catch(c.Run, 'test-a/b', virt_mode='job', memory_limit="1G"), porto.exceptions.InvalidValue)

b = c.Run('test-a/b', weak=False, virt_mode='job', command='sleep 1000')
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


# host/job

a = c.Run('test-a', virt_mode='host', memory_limit="1G")
ExpectEq(a['state'], 'meta')

b = c.Run('test-a/b', virt_mode='job', command='sleep 1000')
ExpectEq(b['state'], 'running')
b.Destroy()

b = c.Run('test-a/b', virt_mode='job', command='sleep 1000')
ExpectEq(b['state'], 'running')
b.Destroy()

ExpectEq(a['state'], 'meta')
a.Destroy()
