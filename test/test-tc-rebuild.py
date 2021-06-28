import porto
import subprocess
import test_common
from test_common import *
import re
import os

# disabled because tc do not used
sys.exit(0)

conn = porto.Connection()

expected_errors = int(conn.GetData('/', 'porto_stat[errors]'))
expected_warnings = int(conn.GetData('/', 'porto_stat[warnings]'))

def has_qdisc(link):
    return re.match('qdisc (htb|hfsc) ', subprocess.check_output(['tc', 'qdisc', 'show', 'dev', link]))

def del_qdisc(link):
    subprocess.check_call(['tc', 'qdisc', 'del', 'root', 'dev', link])

ConfigurePortod('test-tc-rebuild', """
network {
    enable_host_net_classes: true,
    default_qdisc: "default: codel",
    container_qdisc: "default: fq_codel",
}""")

try:
    managed_links = [link for link in os.listdir('/sys/class/net') if has_qdisc(link)]
    assert managed_links

    for link in managed_links:
        del_qdisc(link)
        assert not has_qdisc(link)

# start test container
    test = conn.Create('test')
    test.SetProperty('command', 'true')
    test.Start()
    test.Wait()
    assert test.GetData('exit_status') == '0'
    test.Destroy()

# recheck qdisc
    for link in managed_links:
        assert has_qdisc(link)

    ExpectEq(int(conn.GetData('/', 'porto_stat[errors]')), expected_errors)
    ExpectEq(int(conn.GetData('/', 'porto_stat[warnings]')), expected_warnings)

finally:
    ConfigurePortod('test-tc-rebuild', "")
