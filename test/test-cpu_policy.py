import porto
from test_common import ExpectEq, ExpectNe
import multiprocessing

CPUNR = multiprocessing.cpu_count()

def parse_task_affinity(ct):
    pid = ct.GetProperty('root_pid')
    return [x for x in open('/proc/%s/status' % pid).readlines()
            if 'Cpus_allowed_list' in x][0].split()[1].strip()

c = porto.Connection()
ct = c.CreateWeakContainer('test-cpu_policy')

smt_enabled = open("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list").read().strip() != "0"

# Testcase 1: just set and start
ct.SetProperty('cpu_policy', 'nosmt')
ct.SetProperty('command', 'sleep 1234')
ct.Start()

if smt_enabled:
    ExpectNe(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))
else:
    ExpectEq(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))


# Testcase 2: apply dynamically
ct.SetProperty('cpu_policy', 'normal')
ExpectEq(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))
ct.SetProperty('cpu_policy', 'nosmt')
if smt_enabled:
    ExpectNe(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))
else:
    ExpectEq(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))


# Testcase 3: test inheritance on start
ct2 = c.CreateWeakContainer('test-cpu_policy/sub')
ct2.SetProperty('command', "sleep 4321")
ct2.Start()
if smt_enabled:
    ExpectNe(parse_task_affinity(ct2), "0-%d" % (CPUNR - 1))
else:
    ExpectEq(parse_task_affinity(ct2), "0-%d" % (CPUNR - 1))
ct.Stop()


# Testcase 4: ensure non-inheritance in dynamic
if smt_enabled:
    ct.SetProperty('cpu_policy', 'normal')
    ct.Start()
    ct2.Start()

    ExpectEq(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))
    ExpectEq(parse_task_affinity(ct2), "0-%d" % (CPUNR - 1))

    ct.SetProperty('cpu_policy', 'nosmt')
    ExpectNe(parse_task_affinity(ct), "0-%d" % (CPUNR - 1))
    ExpectEq(parse_task_affinity(ct2), "0-%d" % (CPUNR - 1))

    ct.Stop()

# Testcase 5: check against cpu_sets / jails
if smt_enabled:
    ct.SetProperty('cpu_set', '0,%d' % (CPUNR / 2))
    ct.Start()
    ExpectEq(parse_task_affinity(ct), "0")

    ct.Stop()
    ct.SetProperty('cpu_set', 'jail 2')
    ct.Start()
    ExpectEq(parse_task_affinity(ct), "0")

    ct2.Start()
    ExpectEq(parse_task_affinity(ct2), "0")

    ct.Stop()

    if CPUNR > 2:
        ct.SetProperty('cpu_set', 'jail 4')
        ct.Start()
        ExpectEq(parse_task_affinity(ct), "0-1")
        ct2.Start()
        ExpectEq(parse_task_affinity(ct2), "0-1")

ct.Destroy()
