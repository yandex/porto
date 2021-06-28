import porto
from test_common import *
import os

# disabled because tc do not used
sys.exit(0)

conn = porto.Connection()

expected_errors = int(conn.GetData('/', 'porto_stat[errors]'))
expected_warnings = int(conn.GetData('/', 'porto_stat[warnings]'))

has_priority = os.path.exists('/sys/fs/cgroup/net_cls/net_cls.priority')

def ping(count=1, size=56, tos=0, host='2a02:6b8::2:242'):
    return "ping6 -c {} -s {} -W 1 -i 0.2 -n -Q {} {}".format(count, size, tos, host)

ConfigurePortod('test-tc-classes', """
network {
    enable_host_net_classes: true,
    default_qdisc: "default: codel",
    container_qdisc: "default: fq_codel",
}""")

try:
    a = conn.Run("a", wait=10, command=ping())
    ExpectEq(a.GetProperty('net_packets[Uplink]', sync=True), '1')
    ExpectProp(a, 'net_bytes[Uplink]', '118')
    for cs in range(0, 8):
        ExpectProp(a, 'net_packets[CS{}]'.format(cs), '1' if cs == 0 else '0')
        ExpectProp(a, 'net_packets[Leaf CS{}]'.format(cs), '1' if cs == 0 else '0')
        ExpectProp(a, 'net_bytes[CS{}]'.format(cs), '118' if cs == 0 else '0')
        ExpectProp(a, 'net_bytes[Leaf CS{}]'.format(cs), '118' if cs == 0 else '0')
    a.Destroy()


    a = conn.Run("a", wait=10, command=ping(tos='0x40', count=10), net_tos='CS0' if has_priority else 'CS2')
    ExpectEq(a.GetProperty('net_packets[Uplink]', sync=True), '10')
    for cs in range(0, 8):
        ExpectProp(a, 'net_packets[CS{}]'.format(cs), '10' if cs == 2 else '0')
        ExpectProp(a, 'net_packets[Leaf CS{}]'.format(cs), '10' if cs == 2 else '0')
        ExpectProp(a, 'net_bytes[CS{}]'.format(cs), '1180' if cs == 2 else '0')
        ExpectProp(a, 'net_bytes[Leaf CS{}]'.format(cs), '1180' if cs == 2 else '0')
    a.Destroy()


    b = conn.Run("b")
    a = conn.Run("b/a", wait=10, command=ping())
    ExpectEq(a.GetProperty('net_packets[Uplink]', sync=True), '1')
    ExpectProp(b, 'net_packets[Uplink]', '1')
    for cs in range(0, 8):
        ExpectProp(a, 'net_packets[CS{}]'.format(cs), '1' if cs == 0 else '0')
        ExpectProp(b, 'net_packets[CS{}]'.format(cs), '1' if cs == 0 else '0')
        ExpectProp(a, 'net_bytes[CS{}]'.format(cs), '118' if cs == 0 else '0')
        ExpectProp(b, 'net_bytes[CS{}]'.format(cs), '118' if cs == 0 else '0')

    a.Destroy()
    ExpectEq(b.GetProperty('net_packets[Uplink]', sync=True), '1')
    for cs in range(0, 8):
        ExpectProp(b, 'net_packets[CS{}]'.format(cs), '1' if cs == 0 else '0')
        ExpectProp(b, 'net_bytes[CS{}]'.format(cs), '118' if cs == 0 else '0')
    b.Destroy()

    a = conn.Run("a", wait=10, command=ping(count=10), net_limit="default: 1")
    ExpectEq(a.GetProperty('net_packets[Uplink]', sync=True), '0')
    for cs in range(0, 8):
        ExpectProp(a, 'net_packets[CS{}]'.format(cs), '0')
        ExpectProp(a, 'net_packets[Leaf CS{}]'.format(cs), '0')
        ExpectProp(a, 'net_bytes[CS{}]'.format(cs), '0')
        ExpectProp(a, 'net_bytes[Leaf CS{}]'.format(cs), '0')
    a.Destroy()


    ExpectEq(int(conn.GetData('/', 'porto_stat[errors]')), expected_errors)
    ExpectEq(int(conn.GetData('/', 'porto_stat[warnings]')), expected_warnings)

finally:
    ConfigurePortod('test-tc-classes', "")
