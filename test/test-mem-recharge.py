import porto
import sys
from test_common import *


if not os.path.exists('/sys/fs/cgroup/memory/memory.recharge_on_pgfault'):
    print "Test not applicable"
    sys.exit(0)

may_not_recharge_orphan_locked = get_kernel_maj_min() < (4, 19)

conn = porto.Connection()
ct = conn.CreateWeakContainer("test-mem-recharge")
vol = conn.CreateVolume(private="test-mem-recharge", containers=ct.name)

ct.SetProperty("cwd", vol.path)
ct.SetProperty("command", "vmtouch -t test")

ct_mlock = conn.CreateWeakContainer("test-mem-recharge-mlock")
ct_mlock.SetProperty("cwd", vol.path)
ct_mlock.SetProperty("command", "vmtouch -l -d -w test")

ct2 = conn.CreateWeakContainer("test-mem-recharge-second")
ct2.SetProperty("cwd", vol.path)
ct2.SetProperty("command", "vmtouch -t test")

size = 100 << 20
delta = 10 << 20
file(vol.path + "/test", 'w').truncate(size)

def Run(ct):
    ct.Start()
    ct.Wait(60000)
    ExpectProp(ct, "state", "dead")
    ExpectProp(ct, "exit_code", "0")

ExpectProp(ct, "recharge_on_pgfault", False)
ExpectProp(ct2, "recharge_on_pgfault", False)

print '- first touch'
Run(ct)
ExpectPropGe(ct, "memory_usage", size)

print '- second touch'
Run(ct2)
ExpectPropGe(ct, "memory_usage", size)
ExpectPropLe(ct2, "memory_usage", delta)
ct2.Stop()

ct2.SetProperty("recharge_on_pgfault", True)

print '- recharge'
Run(ct2)
ExpectPropGe(ct2, "memory_usage", size)
ExpectPropLe(ct, "memory_usage", delta)
ct.Stop()

print '- touch recharged'
Run(ct)
ExpectPropGe(ct2, "memory_usage", size)
ExpectPropLe(ct, "memory_usage", delta)
ct.Stop()

ct.SetProperty("recharge_on_pgfault", True)

print '- recharge recharged'
Run(ct)
if get_kernel_maj_min() > (3, 18):
    ExpectPropLe(ct, "memory_usage", delta)
    ExpectPropGe(ct2, "memory_usage", size)
else:
    print 'XFAIL old kernel'
    ExpectPropLe(ct2, "memory_usage", delta)
    ExpectPropGe(ct, "memory_usage", size)
ct.Stop()

ct2.Stop()

print '- first recharge orphan'
Run(ct)
ExpectPropGe(ct, "memory_usage", size)
ct.Stop()

print '- second recharge orphan'
Run(ct2)
ExpectPropGe(ct2, "memory_usage", size)
ct2.Stop()

ct.SetProperty("recharge_on_pgfault", False)

print '- touch orphan'
Run(ct)
if may_not_recharge_orphan_locked:
    ExpectPropLe(ct, "memory_usage", delta)
else:
    ExpectPropGe(ct, "memory_usage", size)
ct.Stop()

ct2.SetProperty("recharge_on_pgfault", False)

print '- second touch orphan'
Run(ct2)
if may_not_recharge_orphan_locked:
    ExpectPropLe(ct2, "memory_usage", delta)
else:
    ExpectPropGe(ct2, "memory_usage", size)

ct2.Stop()

ct.SetProperty("recharge_on_pgfault", True)

ct.SetProperty("memory_limit", size)

print '- hit limit'
Run(ct)
ExpectPropGe(ct, "memory_usage", size - delta)
ct.Stop()

ct.SetProperty("memory_limit", delta)

print '- hit harder'
Run(ct)
ExpectPropLe(ct, "memory_usage", delta)
ct.Stop()

ct.SetProperty("memory_limit", size + delta * 2)

Run(ct)
ExpectPropGe(ct, "memory_usage", size)

ct_mlock.SetProperty("memory_limit", size + delta)

print '- mlock smaller'
Run(ct_mlock)
if may_not_recharge_orphan_locked:
    ExpectPropLe(ct_mlock, "memory_usage", delta)
    ExpectPropGe(ct, "memory_usage", size)
else:
    ExpectPropLe(ct, "memory_usage", delta)
    ExpectPropGe(ct_mlock, "memory_usage", size)
ct_mlock.Stop()

ct_mlock.SetProperty("memory_limit", size + delta * 3)

print '- mlock bigger'
Run(ct_mlock)
ExpectPropGe(ct_mlock, "memory_usage", size)
ExpectPropLe(ct, "memory_usage", delta)
ct_mlock.Stop()
ct.Stop()

print '- mlock orphan'
Run(ct_mlock)
if get_kernel_maj_min() > (3, 18):
    ExpectPropGe(ct_mlock, "memory_usage", size)
else:
    print 'XFAIL old kernel'
    ExpectPropLe(ct_mlock, "memory_usage", delta)
ct_mlock.Stop()

ct_mlock.SetProperty("recharge_on_pgfault", True)

print '- recharge mlock orphan'
Run(ct_mlock)
ExpectPropGe(ct_mlock, "memory_usage", size)
ct_mlock.Stop()
