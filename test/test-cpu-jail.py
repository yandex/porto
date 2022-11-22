import multiprocessing
import porto
import sys

from test_common import *

CPUNR = multiprocessing.cpu_count()
if CPUNR < 3:
    sys.exit(0)

conn = porto.Connection(timeout=30)
a = None
b = None
c = None

def check_affinity(ct, affinity):
    ct_affinity = ct['cpu_set_affinity']
    with open('/sys/fs/cgroup/cpuset/porto%{}/cpuset.cpus'.format(ct)) as f:
        cg_affinity = str(f.read()).strip()
    if affinity.startswith('!'):
        assert ct_affinity != affinity, '{} != {}'.format(ct_affinity, affinity)
        assert cg_affinity != affinity, '{} != {}'.format(cg_affinity, affinity)
    else:
        assert ct_affinity == affinity, '{} == {}'.format(ct_affinity, affinity)
        assert cg_affinity == affinity, '{} == {}'.format(cg_affinity, affinity)

try:
    # incorrect values

    a = conn.Create('a')
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail')
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail 0')
    a.SetProperty('cpu_set', 'jail {}'.format(CPUNR))
    ExpectException(a.Start, porto.exceptions.ResourceNotAvailable)
    a.SetProperty('cpu_set', '')
    a.Start()

    b = conn.Create('a/b')
    b.SetProperty('cpu_set', 'jail 2')
    b.Start()

    check_affinity(a, '!0')
    check_affinity(b, '0-1')

    ExpectException(a.SetProperty, porto.exceptions.ResourceNotAvailable, 'cpu_set', 'jail 1')
    ExpectException(a.SetProperty, porto.exceptions.ResourceNotAvailable, 'cpu_set', 'jail 2')
    ExpectException(a.SetProperty, porto.exceptions.ResourceNotAvailable, 'cpu_set', 'jail 3')

    b.Destroy()
    a.Destroy()

    # reset jail on stopped container

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 2')
    assert a['cpu_set'] == 'jail 2'
    a.SetProperty('cpu_set', '')
    assert a['cpu_set'] == ''
    a.SetProperty('cpu_set', 'jail 2')

    b = conn.Create('b')
    b.SetProperty('cpu_set', 'jail 2')

    # simple

    a.Start()
    assert a['cpu_set'] == 'jail 2'
    check_affinity(a, '0-1')

    b.Start()
    assert b['cpu_set'] == 'jail 2'
    if CPUNR >= 4:
        check_affinity(b, '2-3')

    # destroy first jailed container

    a.Destroy()

    # other containers stay untouched

    assert b['cpu_set'] == 'jail 2'
    if CPUNR >= 4:
        check_affinity(b, '2-3')

    # free cores are reused

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 1')
    a.Start()
    assert a['cpu_set'] == 'jail 1'
    check_affinity(a, '0')

    # increase jail value, cores still reused

    a.SetProperty('cpu_set', 'jail 2')
    check_affinity(a, '0-1')

    # skip already used cores

    a.SetProperty('cpu_set', 'jail 3')
    if CPUNR >= 5:
        check_affinity(a, '0-1,4')

    # reload

    ReloadPortod()

    assert a['cpu_set'] == 'jail 3'
    if CPUNR >= 5:
        check_affinity(a, '0-1,4')

    assert b['cpu_set'] == 'jail 2'
    if CPUNR >= 4:
        check_affinity(b, '2-3')

    b.Destroy()
    a.Destroy()

    # jail + numa -> numa -> jail -> jail + numa

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 2; node 0')
    a.Start()
    assert a['cpu_set'] == 'jail 2; node 0'
    check_affinity(a, '0-1')
    a.SetProperty('cpu_set', 'node 0')
    check_affinity(a, '!0-1')
    a.SetProperty('cpu_set', 'jail 2')
    check_affinity(a, '0-1')
    a.SetProperty('cpu_set', 'jail 2; node 0')
    assert a['cpu_set'] == 'jail 2; node 0'
    check_affinity(a, '0-1')

    ReloadPortod()

    assert a['cpu_set'] == 'jail 2; node 0'
    check_affinity(a, '0-1')

    # jail -> raw cores -> jail -> none

    a.SetProperty('cpu_set', 'jail 1')
    assert a['cpu_set'] == 'jail 1'
    check_affinity(a, '0')

    a.SetProperty('cpu_set', '1')
    assert a['cpu_set'] == '1'
    check_affinity(a, '1')

    a.SetProperty('cpu_set', 'jail 1')
    assert a['cpu_set'] == 'jail 1'
    check_affinity(a, '1')

    a.SetProperty('cpu_set', '')
    assert a['cpu_set'] == ''
    check_affinity(a, '0-{}'.format(CPUNR - 1))

    a.Destroy()

    # PORTO-952

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 1')
    a.Start()

    b = conn.Create('a/b')
    b.SetProperty('cpu_set', 'jail 1')
    ExpectException(b.Start, porto.exceptions.ResourceNotAvailable)
    b.SetProperty('cpu_set', '')
    assert b['cpu_set'] == ''
    b.Start()

    c = conn.Create('c')
    c.Start()

    assert a['cpu_set'] == 'jail 1'
    check_affinity(a, '0')

    c.Destroy()
    b.Destroy()
    a.Destroy()

    # PORTO-992

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 2')
    a.Start()

    b = conn.Create('a/b')
    b.SetProperty('controllers', 'cpuset')
    b.Start()

    c = conn.Create('a/b/c')
    c.SetProperty('controllers', 'cpuset')
    c.Start()

    check_affinity(a, '0-1')
    check_affinity(b, '0-1')
    check_affinity(c, '0-1')

    a.SetProperty('cpu_set', 'jail 1')

    check_affinity(a, '0')
    check_affinity(b, '0')
    check_affinity(c, '0')

    a.SetProperty('cpu_set', 'jail 3')

    check_affinity(a, '0-2')
    check_affinity(b, '0-2')
    check_affinity(c, '0-2')

    c.Destroy()
    b.Destroy()
    a.Destroy()

    # PORTO-1074

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'node 0; jail 2')
    a.Start()

    b = conn.Create('b')
    b.SetProperty('cpu_set', 'node 0; jail 2')
    b.Start()

    check_affinity(a, '0-1')
    check_affinity(b, '2-3')

    b.SetProperty('cpu_set', 'node 0')
    check_affinity(a, '0-1')
    check_affinity(b, '0-{}'.format(CPUNR - 1))

    b.SetProperty('cpu_set', 'node 0; jail 2')
    check_affinity(a, '0-1')
    check_affinity(b, '2-3')

    b.Destroy()
    a.Destroy()

finally:
    for ct in [a, b, c]:
        try:
            ct.Destroy()
        except:
            pass
