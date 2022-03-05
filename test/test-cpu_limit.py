import porto
import os
import multiprocessing
import types
from test_common import *

NAME = os.path.basename(__file__)

def CT_NAME(suffix):
    global NAME
    return NAME + "-" + suffix

EPS = 0.95
TEST_LIM_SHARE = 0.75 #of the whole machine

DURATION = 1000 #ms
CPUNR = multiprocessing.cpu_count()
HAS_RT_LIMIT = os.access("/sys/fs/cgroup/cpu/cpu.rt_runtime_us", os.F_OK)
TEST_CORES_SHARE = CPUNR * TEST_LIM_SHARE

print "Available cores: {}, using EPS {}, run duration {} ms".format(CPUNR, EPS, DURATION)

def PrepareCpuContainer(ct, guarantee, limit, rt = False):
    if (rt):
        ct.SetProperty("cpu_policy", "rt")
    else:
        ct.SetProperty("cpu_policy", "normal")

    if (limit != 0.0):
        ct.SetProperty("cpu_limit", "{}c".format(limit))

    if (guarantee != 0.0):
        ct.SetProperty("cpu_guarantee", "{}c".format(guarantee))

    ct.SetProperty("cwd", os.getcwd())
    ct.SetProperty("command", "bash -c \'read; stress -c {} -t {}\'"\
                              .format(CPUNR, DURATION / 1000))
    ct.Limit = limit
    ct.Guarantee = guarantee
    ct.Eps = EPS

    #Adjust EPS for low limits
    if limit:
        ct.Eps = min(limit * 0.75, ct.Eps)
    elif guarantee:
        ct.Eps = min(guarantee * 0.75, ct.Eps)

    (ct.fd1, ct.fd2) = os.pipe()
    ct.SetProperty("stdin_path", "/dev/fd/{}".format(ct.fd1))

    ct.Start()

    return ct

def CheckCpuContainer(ct):
    assert ct.Wait(DURATION * 2) == ct.name, "container running time twice exceeded "\
                                             "expected duration {}".format(DURATION)

    usage = int(ct.GetProperty("cpuacct.usage")) / (10.0 ** 9) / (DURATION / 1000)

    print "{} : cpuacct usage: {}".format(ct.name, usage)

    assert ct.GetProperty("exit_status") == "0", "stress returned non-zero, stderr: {}"\
                                                .format(ct.GetProperty("stderr"))
    if ct.Limit != 0.0:
        assert usage < (ct.Limit + ct.Eps), "usage {} should be at most {}"\
                                          .format(usage, ct.Limit + ct.Eps)
    if ct.Guarantee != 0.0:
        assert usage > (ct.Guarantee - ct.Eps), "usage {} should be at least {}"\
                                              .format(usage, ct.Guarantee - ct.Eps)
    ct.Destroy()

    return usage

def KickCpuContainer(ct):
    os.close(ct.fd1)
    os.close(ct.fd2)
    return ct

def RunOneCpuContainer(ct):
    ct.Kick()
    ct.Check()
    return ct

def AllocContainer(conn, suffix):
    ct = conn.CreateWeakContainer(CT_NAME(suffix))
    ct.Prepare = types.MethodType(PrepareCpuContainer, ct)
    ct.Check = types.MethodType(CheckCpuContainer, ct)
    ct.Kick = types.MethodType(KickCpuContainer, ct)
    ct.RunOne = types.MethodType(RunOneCpuContainer, ct)
    return ct

def SplitLimit(conn, prefix, total, n, rt = False):
    conts = []
    for i in range(0, n):
        ct = conn.AllocContainer("{}_{}".format(prefix, i))
        conts += [ct.Prepare(0.0, total / n, rt)]

    for ct in conts:
        ct.Kick()

    for ct in conts:
        ct.Check()

conn = porto.Connection(timeout=30)
conn.AllocContainer = types.MethodType(AllocContainer, conn)

print "\nSet 1c limit for single container:"

conn.AllocContainer("normal_one_core").Prepare(0.0, 1.0).RunOne()

if CPUNR > 1:
    print "\nSet 1.5c limit for single container:"
    conn.AllocContainer("normal_one_and_half_core").Prepare(0.0, 1.5).RunOne()

if CPUNR > 2:
    print "\nSet {}c (CPUNR - 1) limit for single container:".format(CPUNR - 1)
    ct = conn.AllocContainer("normal_minus_one_core").Prepare(0.0, float(CPUNR) - 1.0).RunOne()

if HAS_RT_LIMIT:
    print "\nSet 1c limit for single rt container:"
    ct = conn.AllocContainer("rt_one_core").Prepare(0.0, 1.0, rt=True).RunOne()

    if CPUNR > 1:
        print "\nSet 1.5c limit for single rt container:"
        ct = conn.AllocContainer("rt_one_and_half_core").Prepare(0.0, 1.5, rt=True).RunOne()

    if CPUNR > 2:
        print "\nSet {}c (CPUNR - 1) limit for single rt container:".format(CPUNR - 1)
        ct = conn.AllocContainer("rt_minus_one_core").Prepare(0.0, float(CPUNR) - 1.0, rt=True)
        ct.RunOne()

if CPUNR > 1:
    print "\nSet {}c guarantee for 1 of 2 containers:".format(CPUNR * 2 / 3)
    ct1 = conn.AllocContainer("normal_half_0").Prepare(0.0, 0.0)
    ct2 = conn.AllocContainer("normal_half_guaranteed").Prepare(CPUNR * 2 / 3, 0.0)

    ct1.Kick(); ct2.Kick()
    ct1.Check(); ct2.Check()

    print "\nSplit {}c limit equally btwn 2 containers:".format(TEST_CORES_SHARE)
    SplitLimit(conn, "normal_half", TEST_CORES_SHARE, 2, rt = False)

    if HAS_RT_LIMIT:
        print "\nSplit {}c limit equally btwn 2 rt containers:"\
              .format(TEST_CORES_SHARE)
        SplitLimit(conn, "rt_half", TEST_CORES_SHARE, 2, rt = True)

if CPUNR > 2:
    print "\nSet {}c guarantee for 1 of 3 containers:".format(CPUNR / 2)

    conts = []
    conts += [conn.AllocContainer("normal_third_1").Prepare(0.0, 0.0)]
    conts += [conn.AllocContainer("normal_third_2").Prepare(0.0, 0.0)]
    conts += [conn.AllocContainer("normal_third_guaranteed").Prepare(CPUNR / 2, 0.0)]

    for ct in conts:
        ct.Kick()

    for ct in conts:
        ct.Check()

    print "\nSplit {}c limit equally btwn 3 containers:".format(TEST_CORES_SHARE)
    SplitLimit(conn, "normal_third", TEST_CORES_SHARE, 3, rt = False)

    if HAS_RT_LIMIT:
        print "\nSplit {}c limit equally btwn 3 rt containers:"\
              .format(TEST_CORES_SHARE)
        SplitLimit(conn, "rt_third", TEST_CORES_SHARE, 3, rt = True)

    print "\nSet 0.3c limit for 3 containers:"
    conts = []
    for i in range(0, 3):
        conts += [conn.AllocContainer("one_third_{}".format(i)).Prepare(0.0, 0.33)]

    for ct in conts:
        ct.Kick()

    for ct in conts:
        ct.Check()

if CPUNR > 3:
    print "\nSet {}c guarantee for 1 of 4 containers:".format(CPUNR / 2)

    conts = []
    conts += [conn.AllocContainer("normal_quarter_0").Prepare(0.0, 0.0)]
    conts += [conn.AllocContainer("normal_quarter_1").Prepare(0.0, 0.0)]
    conts += [conn.AllocContainer("normal_quarter_2").Prepare(0.0, 0.0)]
    conts += [conn.AllocContainer("normal_quarter_guaranteed").Prepare(CPUNR / 2, 0.0)]

    for ct in conts:
        ct.Kick()

    for ct in conts:
        ct.Check()

    print "\nSplit {}c limit equally btwn 4 containers:".format(TEST_CORES_SHARE)
    SplitLimit(conn, "normal_quarter", TEST_CORES_SHARE, 4, rt = False)

    if HAS_RT_LIMIT:
        print "\nSplit {}c limit equally btwn 4 rt containers:"\
              .format(TEST_CORES_SHARE)
        SplitLimit(conn, "rt_quarter", TEST_CORES_SHARE, 4, rt = True)

    if CPUNR > 3 and HAS_RT_LIMIT:
        print "\nSet 1c guarantee for 3 containers and 1c limit for rt container:"
        conts = []
        for i in range(0, 3):
            conts += [conn.AllocContainer("one_third_{}".format(i)).Prepare(1.0, 0.0)]

        conts += [conn.AllocContainer("rt_guy").Prepare(0.0, 1.0, rt=True)]

        for ct in conts:
            ct.Kick()

        for ct in conts:
            ct.Check()

    if CPUNR > 7 and HAS_RT_LIMIT:
        print "\nSet 1c guarantee and 1.5c limit for 3 containers and 1c limit for 2 rt containers:"
        conts = []

        for i in range(0, 3):
            conts += [conn.AllocContainer("one_third_{}".format(i)).Prepare(1.0, 1.5)]

        for i in range(0, 2):
            conts += [conn.AllocContainer("rt_guy_{}".format(i))\
                                          .Prepare(0.0, 1.0, rt = True)]
        for ct in conts:
            ct.Kick()

        for ct in conts:
            ct.Check()

# check cpu limit/guarantee bound
a = conn.Create('a', weak=True)
a.SetProperty('cpu_limit', '{}c'.format(CPUNR - 1))
a.SetProperty('cpu_guarantee', '6c')

b = conn.Create('a/b', weak=True)
b.SetProperty('cpu_limit', '{}c'.format(CPUNR - 2))
b.SetProperty('cpu_guarantee', '8c')

c = conn.Create('a/b/c', weak=True)
c.SetProperty('cpu_limit', '{}c'.format(CPUNR))
c.SetProperty('cpu_guarantee', '2c')

d = conn.Create('a/b/c/d', weak=True)
d.SetProperty('command', 'sleep 100')

a.Start()
b.Start()
c.Start()
d.Start()

ExpectProp(a, 'cpu_limit_bound', '{}c'.format(CPUNR - 1))
ExpectProp(b, 'cpu_limit_bound', '{}c'.format(CPUNR - 2))
ExpectProp(c, 'cpu_limit_bound', '{}c'.format(CPUNR - 2))
ExpectProp(d, 'cpu_limit_bound', '{}c'.format(CPUNR - 2))

ExpectProp(a, 'cpu_guarantee_bound', '6c')
ExpectProp(b, 'cpu_guarantee_bound', '8c')
ExpectProp(c, 'cpu_guarantee_bound', '8c')
ExpectProp(d, 'cpu_guarantee_bound', '8c')

assert 0 != a.Dump().status.cpu_limit_total
assert '%dc' % a.Dump().status.cpu_limit_total == a.GetProperty('cpu_limit_total')
assert '%dc' % b.Dump().status.cpu_limit_total == b.GetProperty('cpu_limit_total')
assert '%dc' % c.Dump().status.cpu_limit_total == c.GetProperty('cpu_limit_total')
assert '%dc' % d.Dump().status.cpu_limit_total == d.GetProperty('cpu_limit_total')
a.Destroy()

a = None
b = None

try:
    # check cpu_limit_scale

    def get_cpuacct_knob(ct, knob):
        if ct == '/':
            path = knob
        else:
            path = 'porto%{}/{}'.format(ct, knob)
        with open('/sys/fs/cgroup/cpuacct/{}'.format(path)) as f:
            return int(f.read().decode('utf-8').strip())

    def get_cfs_quota_us(ct):
        return get_cpuacct_knob(ct, 'cpu.cfs_quota_us')

    cfs_period_us = get_cpuacct_knob('/', 'cpu.cfs_period_us')

    limit_cores = 2

    ConfigurePortod('test-cpu_limit', "")

    a = conn.Create('a')
    a.SetProperty('cpu_limit', '{}c'.format(limit_cores))
    a.Start()

    ExpectProp(a, 'cpu_limit', '{}c'.format(limit_cores))
    ExpectEq(get_cfs_quota_us('a'), limit_cores * cfs_period_us)

    def run_scale_test(limit_scale):
        global limit_cores
        global cfs_period_us

        ConfigurePortod('test-cpu_limit', """
    container {
        cpu_limit_scale: %f
    }
    """ % (limit_scale))

        ExpectProp(a, 'cpu_limit', '{}c'.format(limit_cores))
        ExpectEq(get_cfs_quota_us('a'), int(limit_scale * limit_cores * cfs_period_us))

    run_scale_test(1.1)
    run_scale_test(0.9)

    ConfigurePortod('test-cpu_limit', """
    container {
        cpu_limit_scale: 0
    }
    """)

    ExpectProp(a, 'cpu_limit', '{}c'.format(limit_cores))
    ExpectEq(get_cfs_quota_us('a'), -1)

    ConfigurePortod('test-cpu_limit', "")

    ExpectProp(a, 'cpu_limit', '{}c'.format(limit_cores))
    ExpectEq(get_cfs_quota_us('a'), limit_cores * cfs_period_us)

    a.Destroy()

    # check proportional_cpu_shares

    def get_cpu_shares(ct):
        return get_cpuacct_knob(ct, 'cpu.shares')

    def get_cfs_reserve_shares(ct):
        return get_cpuacct_knob(ct, 'cpu.cfs_reserve_shares')

    base_shares = get_cpuacct_knob('/', 'cpu.shares')
    min_shares = 2

    guarantee_cores = 2

    ConfigurePortod('test-cpu_limit', "")

    a = conn.Create('a')
    b = conn.Create('b')
    a.SetProperty('cpu_guarantee', '{}c'.format(guarantee_cores))
    b.SetProperty('cpu_guarantee', '0c')
    a.Start()
    b.Start()

    ExpectProp(a, 'cpu_guarantee', '{}c'.format(guarantee_cores))
    ExpectEq(get_cpu_shares('a'), base_shares)
    ExpectEq(get_cfs_reserve_shares('a'), 16 * base_shares)

    ExpectProp(b, 'cpu_guarantee', '0c')
    ExpectEq(get_cpu_shares('b'), base_shares)
    ExpectEq(get_cfs_reserve_shares('b'), base_shares)

    ConfigurePortod('test-cpu_limit', """
    container {
        proportional_cpu_shares: true
    }
    """)

    ExpectProp(a, 'cpu_guarantee', '{}c'.format(guarantee_cores))
    ExpectEq(get_cpu_shares('a'), guarantee_cores * base_shares)
    ExpectEq(get_cfs_reserve_shares('a'), guarantee_cores * base_shares)

    ExpectProp(b, 'cpu_guarantee', '0c')
    ExpectEq(get_cpu_shares('b'), min_shares)
    ExpectEq(get_cfs_reserve_shares('b'), min_shares)

    ConfigurePortod('test-cpu_limit', "")

    ExpectProp(a, 'cpu_guarantee', '{}c'.format(guarantee_cores))
    ExpectEq(get_cpu_shares('a'), base_shares)
    ExpectEq(get_cfs_reserve_shares('a'), 16 * base_shares)

    ExpectProp(b, 'cpu_guarantee', '0c')
    ExpectEq(get_cpu_shares('b'), base_shares)
    ExpectEq(get_cfs_reserve_shares('b'), base_shares)

finally:
    for ct in [a, b]:
        try:
            ct.Destroy()
        except:
            pass
