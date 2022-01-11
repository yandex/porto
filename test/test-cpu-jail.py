import multiprocessing
import porto
import sys

from test_common import *

CPUNR = multiprocessing.cpu_count()
if CPUNR < 3:
    sys.exit(0)

conn = porto.Connection(timeout=30)
a = conn.Create('a')
b = conn.Create('b')
c = None

try:
    # incorrect values

    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail')
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail 0')
    a.SetProperty('cpu_set', 'jail {}'.format(CPUNR))
    ExpectException(a.Start, porto.exceptions.ResourceNotAvailable)

    # reset jail on stopped container

    a.SetProperty('cpu_set', 'jail 2')
    assert a['cpu_set'] == 'jail 2'
    a.SetProperty('cpu_set', '')
    assert a['cpu_set'] == ''

    a.SetProperty('cpu_set', 'jail 2')
    b.SetProperty('cpu_set', 'jail 2')

    # simple

    a.Start()
    assert a['cpu_set'] == 'jail 2'
    assert a['cpu_set_affinity'] == '0-1'

    b.Start()
    assert b['cpu_set'] == 'jail 2'
    if CPUNR >= 4:
        assert b['cpu_set_affinity'] == '2-3'

    # destroy first jailed container

    a.Destroy()

    # other containers stay untouched

    assert b['cpu_set'] == 'jail 2'
    if CPUNR >= 4:
        assert b['cpu_set_affinity'] == '2-3'

    # free cores are reused

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 1')
    a.Start()
    assert a['cpu_set'] == 'jail 1'
    assert a['cpu_set_affinity'] == '0'

    # increase jail value, cores still reused

    a.SetProperty('cpu_set', 'jail 2')
    assert a['cpu_set_affinity'] == '0-1'

    # skip already used cores

    a.SetProperty('cpu_set', 'jail 3')
    if CPUNR >= 5:
        assert a['cpu_set_affinity'] == '0-1,4'

    # reload

    ReloadPortod()

    assert a['cpu_set'] == 'jail 3'
    if CPUNR >= 5:
        assert a['cpu_set_affinity'] == '0-1,4'

    assert b['cpu_set'] == 'jail 2'
    if CPUNR >= 4:
        assert b['cpu_set_affinity'] == '2-3'

    b.Destroy()
    a.Destroy()

    # jail + numa -> numa -> jail -> jail + numa

    a = conn.Create('a')
    a.SetProperty('cpu_set', 'jail 2; node 0')
    a.Start()
    assert a['cpu_set'] == 'jail 2; node 0'
    assert a['cpu_set_affinity'] == '0-1'
    a.SetProperty('cpu_set', 'node 0')
    assert a['cpu_set_affinity'] != '0-1'
    a.SetProperty('cpu_set', 'jail 2')
    assert a['cpu_set_affinity'] == '0-1'
    a.SetProperty('cpu_set', 'jail 2; node 0')
    assert a['cpu_set'] == 'jail 2; node 0'
    assert a['cpu_set_affinity'] == '0-1'

    ReloadPortod()

    assert a['cpu_set'] == 'jail 2; node 0'
    assert a['cpu_set_affinity'] == '0-1'

    # jail -> raw cores -> jail -> none

    a.SetProperty('cpu_set', 'jail 1')
    assert a['cpu_set'] == 'jail 1'
    assert a['cpu_set_affinity'] == '0'

    a.SetProperty('cpu_set', '1')
    assert a['cpu_set'] == '1'
    assert a['cpu_set_affinity'] == '1'

    a.SetProperty('cpu_set', 'jail 1')
    assert a['cpu_set'] == 'jail 1'
    assert a['cpu_set_affinity'] == '1'

    a.SetProperty('cpu_set', '')
    assert a['cpu_set'] == ''
    assert a['cpu_set_affinity'] == '0-{}'.format(CPUNR - 1)

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
    assert a['cpu_set_affinity'] == '0'

finally:
    for ct in [a, b, c]:
        try:
            ct.Destroy()
        except:
            pass
