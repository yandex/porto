import porto
import subprocess
import test_common
from test_common import *
import re

conn = porto.Connection()

expected_errors = int(conn.GetData('/', 'porto_stat[errors]'))
expected_warnings = int(conn.GetData('/', 'porto_stat[warnings]'))

# get managed links
links = [l.split(':')[0].strip() for l in conn.GetData('/', 'net_bytes').split(';') if l]
assert links

def check_qdisc(link):
    return re.match('qdisc (htb|hfsc) ', subprocess.check_output(['tc', 'qdisc', 'show', 'dev', link]))

def del_qdisc(link):
    subprocess.check_call(['tc', 'qdisc', 'del', 'root', 'dev', link])

# check and delete qdisc
for link in links:
    assert check_qdisc(link)
    del_qdisc(link)
    assert not check_qdisc(link)
    expected_warnings += 2

# start test container
test = conn.Create('test')
test.SetProperty('command', 'true')
test.Start()
test.Wait()
assert test.GetData('exit_status') == '0'
test.Destroy()

# recheck qdisc
for link in links:
    assert check_qdisc(link)

assert int(conn.GetData('/', 'porto_stat[errors]')) == expected_errors
assert int(conn.GetData('/', 'porto_stat[warnings]')) == expected_warnings
