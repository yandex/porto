import porto
import json
import time
import os
import sys
import subprocess
import re
import collections
from test_common import *

remote_host = os.environ.get('PORTO_IPERF3_SERVER', None)

if not remote_host:
    print "No remote host specified, skipping test"
    sys.exit(0)

server1=(remote_host, 5201)
server2=(remote_host, 5202)
local_server=("fd00::1", 5201)

dev = "eth0"
qdisc = "htb"
rate = 10


def teardown_dummy():
    subprocess.check_output(["ip", "link", "delete", "dummy-porto"])

def setup_dummy():
    try:
        teardown_dummy()
    except:
        pass
    subprocess.check_output(["ip", "link", "add", "dummy-porto", "type", "dummy"])
    subprocess.check_output(["ip", "address", "add", "fd00::1/64", "dev", "dummy-porto"])

def setup_all_forwarding():
    subprocess.check_output(["sysctl", "-w", "net.ipv6.conf.all.forwarding=1"])

def qdisc_is_classful():
    return qdisc == "htb" or qdisc == "hfsc"

def get_tx_queues_num():
    queues = os.listdir("/sys/class/net/eth0/queues/")
    return len([q for q in queues if q.startswith("tx")])

tx_queues_num = get_tx_queues_num()

def check_expected_qdisc():
    qdisc_pattern = """
        ^qdisc\s+
        (?P<qdisc>[a-z-_]+)\s+
        (?P<handle>[a-f\d\:]+)\s+
        .*$"""
    qdisc_re = re.compile(qdisc_pattern, re.X)

    qdiscs = collections.defaultdict(list)
    output = subprocess.check_output(["tc", "qdisc", "show", "dev", dev])
    for line in output.split(b'\n'):
        m = qdisc_re.match(line)
        if not m or len(m.groups()) != 2:
            continue
        result = m.groupdict()
        qdiscs[result.pop("qdisc")].append(result.pop("handle"))

    if qdisc_is_classful():
        Expect(len(qdiscs) == 2)
        Expect(len(qdiscs[qdisc]) == 1)
    else:
        if tx_queues_num > 1:
            Expect(len(qdiscs) == 2)
            Expect(len(qdiscs["mq"]) == 1)
        else:
            Expect(len(qdiscs) == 1)
        Expect(len(qdiscs[qdisc])) == tx_queues_num

def get_tc_classes():
    classes = set()

    class_pattern = """
        ^class\s+
        %s\s+
        (?P<class>[a-f\d\:]+)\s+
        .*$""" % qdisc
    class_re = re.compile(class_pattern, re.X)

    output = subprocess.check_output(["tc", "class", "show", "dev", dev])
    for line in output.split(b'\n'):
        m = class_re.match(line)
        if not m or len(m.groups()) != 1:
            continue
        result = m.groupdict()
        classes.add(result.pop("class"))

    return classes

def get_porto_net_classes(ct):
    return set(str(cls.split(': ')[1]) for cls in ct.GetProperty("net_class_id").split(';'))

def check_porto_net_classes(*cts):
    tc_classes = get_tc_classes()
    for ct in cts:
        ct_classes = get_porto_net_classes(ct)
        Expect(ct_classes.issubset(tc_classes))

def run_iperf_client(name, server, wait=None, mtn=False, cs=None, reverse=False, cfg={}, **kwargs):
    command="iperf3 --client " + server[0]
    command += " --port " + str(server[1])

    if cs is not None:
        command += " --tos " + str(hex((cs & 7) << 6))

    for k,v in kwargs.items():
        command += " --" + k.replace('_', '-')
        if v is not None:
            command += "=" + str(v)

    command += " --json"
    if reverse:
        command += " --reverse"

    print command

    net = "inherited"
    ip = ""
    if mtn:
        net = "L3 veth"
        ip = "veth fd00::100/128"

    ct = conn.Run(name, command=command, wait=wait, net=net, ip=ip, **cfg)
    if wait:
        # check classes of dead container
        if qdisc_is_classful():
            check_porto_net_classes(ct)
        else:
            ExpectProp(ct, "net_class_id", "1:0")
    return ct

def run_iperf_server(name):
    command = "iperf3 --server --bind fd00::1"
    print command
    return conn.Run(name, command=command)

def bps(ct):
    ct.WaitContainer(5)
    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bits_per_second'] / 2.**23


def run_host_limit_test():
    print "Test net_limit through qdisc classes"

    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: 0"})
    res = bps(a)
    print "net_limit inf -> ", res
    ExpectRange(res, rate * 0.8, rate * 1.33)

    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: %sM" % int(rate * 0.1)})
    res = bps(a)
    print "net_limit %sM -> " % int(rate * 0.1), res
    ExpectRange(res, rate * 0.05, rate * 0.2)

def run_mtn_limit_test():
    print "Test net_limit in MTN"

    s = run_iperf_server("test-net-s")
    a = run_iperf_client("test-net-a", local_server, time=3, wait=5, mtn=True, cfg={"net_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_limit %sM -> " % rate, res
    ExpectRange(res, rate * 0.9, rate * 1.1)
    s.Destroy()

    print "Test net_rx_limit in MTN"

    s = run_iperf_server("test-net-s")
    a = run_iperf_client("test-net-a", local_server, time=3, wait=5, mtn=True, reverse=True, cfg={"net_rx_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_rx_limit %sM -> " % rate, res
    ExpectLe(res, rate * 1.1)
    s.Destroy()

    print "Test both net_limit and net_rx_limit in MTN"

    s = run_iperf_server("test-net-s")
    a = run_iperf_client("test-net-a", local_server, time=3, wait=5, mtn=True, reverse=False, cfg={"net_rx_limit": "default: %sM" % rate, "net_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_limit/net_rx_limit %sM -> " % rate, res
    ExpectLe(res, rate * 1.1)
    s.Destroy()

    s = run_iperf_server("test-net-s")
    a = run_iperf_client("test-net-a", local_server, time=3, wait=5, mtn=True, reverse=True, cfg={"net_rx_limit": "default: %sM" % rate, "net_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_limit/net_rx_limit and reverse %sM -> " % rate, res
    ExpectLe(res, rate * 1.1)
    s.Destroy()


def run_bandwidth_sharing_test():
    b = run_iperf_client("test-net-b", server2, time=5, wait=0, cfg={})
    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={})
    res = bps(a)
    res_b = bps(b)

    b_rate = (5 * res_b - 2 * rate) / 3
    print "net_guarantee 0 and 0 -> %s and %s (%s measured)" % (res, b_rate, res_b)

    # Thresholds rationale:
    # We expect "even" distribution during parallel sending phase, also:
    # 1) Allow up to +- 50% fairness between containers, mind the default rate of 10M
    # 2) Use relative estimation for b container

    ExpectRange(res / b_rate, 0.5, 1.5)

    b = run_iperf_client("test-net-b", server2, time=5, wait=0, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    res = bps(a)
    res_b = bps(b)
    b_rate = (5 * res_b - 2 * rate) / 3
    print "net_guarantee %sM and %sM -> %s and %s (%s measured)" %(int(rate * 0.1), int(rate * 0.1), res, b_rate, res_b)

    # We demand guarantee and also expect "even distribution"
    # 1) no less than -10% from guarantee, 0.09
    # 2) Use relative estimation for b container
    # 3) Allow up to +- 50% fairness between containers, mind the default rate of 10M

    ExpectLe(0.09, res / rate)
    ExpectLe(0.09, res_b / rate)
    ExpectRange(res / b_rate, 0.5, 1.5)

    b = run_iperf_client("test-net-b", server2, time=5, wait=0, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={"net_guarantee": "default: %sM" % int(rate * 0.9)})
    res = bps(a)
    res_b = bps(b)
    b_rate = (5 * res_b - 2 * rate) / 3
    print "net_guarantee %sM and %sM -> %s and %s measured (%s b_rate)" % (int(rate * 0.9), int(rate * 0.1), res, res_b, b_rate)

    # We demand guarantees to be followed
    # 1) no less than -10% from guarantee, 0.8
    # 2) net-b guarantee keeped

    ExpectLe(0.85, res / rate)
    ExpectLe(0.09, res_b / rate)

def run_classful_test(bandwidth_sharing):
    check_expected_qdisc()
    run_host_limit_test()
    run_mtn_limit_test()
    if bandwidth_sharing:
        run_bandwidth_sharing_test()

def run_classless_test():
    check_expected_qdisc()
    ExpectException(run_host_limit_test, AssertionError)
    run_mtn_limit_test()

def set_qdisc(q):
    global qdisc
    qdisc = q
    print "Test %s scheduler" % qdisc

def run_htb_test(bandwidth_sharing=False):
    set_qdisc("htb")
    print "Setup uplink limit %sM" % rate

    ConfigurePortod('test-net-sched', """
network {
    enable_host_net_classes: true
    device_ceil: "default: %sM
    device_rate: "default: %sM"
    device_qdisc: "default: %s"
}
""" % (rate, rate, qdisc))

    run_classful_test(bandwidth_sharing)

def run_hfsc_test(bandwidth_sharing=False):
    set_qdisc("hfsc")
    print "Setup uplink limit %sM" % rate

    ConfigurePortod('test-net-sched', """
network {
    enable_host_net_classes: true
    device_ceil: "default: %sM"
    device_qdisc: "default: %s"
}
""" % (rate, qdisc))

    run_classful_test(bandwidth_sharing)

def run_pfifo_fast_test():
    set_qdisc("pfifo_fast")

    ConfigurePortod('test-net-sched', """
network {
    enable_host_net_classes: false,
    default_qdisc: "default: %s"
}
""" % qdisc)

    run_classless_test()

def run_fq_codel_test():
    set_qdisc("fq_codel")

    ConfigurePortod('test-net-sched', """
network {
    enable_host_net_classes: false,
    default_qdisc: "default: %s"
}
""" % qdisc)

    run_classless_test()


conn = porto.Connection()

try:
    print "Setup dummy iface for MTN tests"
    setup_dummy()

    print "Set net.ipv6.conf.all.forwarding=1"
    setup_all_forwarding()

    # common tests

    run_htb_test(True)
    run_hfsc_test(True)
    run_pfifo_fast_test()
    run_fq_codel_test()

    # test switching

    # htb -> fq_codel -> htb
    run_htb_test()
    run_fq_codel_test()
    run_htb_test()

    # hfsc -> fq_codel -> hfsc
    run_hfsc_test()
    run_fq_codel_test()
    run_hfsc_test()

finally:
    cts = conn.List()
    if 'test-net-a' in cts:
        conn.Destroy('test-net-a')

    if 'test-net-b' in cts:
        conn.Destroy('test-net-b')

    if 'test-net-s' in cts:
        conn.Destroy('test-net-s')

    print "Cleanup uplink limit"
    ConfigurePortod('test-net-sched', "")

    teardown_dummy()
