import os
import porto
from test_common import *

conn = porto.Connection()
ct = conn.CreateWeakContainer("test-dirty-limit")
vol = conn.CreateVolume(private="test-dirty-limit", containers=ct.name)

ct.SetProperty("cwd", vol.path)
ct.SetProperty("command", "dd if=/dev/zero of=test conv=notrunc bs=1M count=100")
ct.SetProperty("dirty_limit", "10M")

if get_kernel_maj_min() > (3, 18):
    dirty = "dirty"
else:
    dirty = "fs_dirty"

def Run():
    ct.Start()
    ct.Wait(60000)
    ExpectProp(ct, "state", "dead")
    ExpectProp(ct, "exit_code", "0")
    ExpectMemoryStatLe(ct, dirty, 10 << 20)
    ExpectEq(os.path.getsize(vol.path + '/test'), 100 << 20)
    ct.Stop()

print '- write'
Run()

print '- rewrite'
Run()

ct.SetProperty("memory_limit", "10M")

print '- hit limit'
Run()

ct.Destroy()
