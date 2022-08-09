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

a = c.Run('test-a', virt_mode='host', memory_limit="1G", weak=False)
ExpectEq(a['state'], 'meta')

b = c.Run('test-a/b', virt_mode='job', command='sleep 1000', weak=False)
ExpectEq(b['state'], 'running')

ReloadPortod()

ExpectEq(a['state'], 'meta')
ExpectEq(b['state'], 'running')

b.Destroy()

b = c.Run('test-a/b', virt_mode='job', command='sleep 1000')
ExpectEq(b['state'], 'running')
b.Destroy()

ExpectEq(a['state'], 'meta')
a.Destroy()

# check controllers after reload.
a = c.Run('test-a', weak=False, command='sleep 1000', memory_limit='256M')

b = c.Run('test-a/b', virt_mode='job', command='sleep 1000', weak=False)
ExpectEq(b['state'], 'running')

oom = c.Run('test-a/c', wait=0, weak=False, command="bash -c 'while true; do stress -m 1 ; done'")
oom.Wait(5)
time.sleep(5)

ExpectEq(a['state'], 'dead')

ExpectEq(len(b['controllers']), 0)

ReloadPortod()

ExpectEq(len(b['controllers']), 0)
ExpectEq(a['state'], 'dead')

a.Destroy()
