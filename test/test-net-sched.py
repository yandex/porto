import porto
import json
import time
import os
import sys
import subprocess
import re
import collections
from test_common import *

remote_host = "fe00::2"

if not remote_host:
    print "No remote host specified, skipping test"
    sys.exit(0)

server1=(remote_host, 5201)
server2=(remote_host, 5202)
local_server=("fd00::1", 5201)

dev = "eth0"
qdisc = "htb"
rate = 10


def teardown_local_veth(conn):
    try:
        conn.Destroy('veth-server1')
    except:
        pass
    try:
        conn.Destroy('veth-server2')
    except:
        pass
    ConfigurePortod('test-net-sched', "")
    subprocess.check_output(["ip6tables", "-t", "nat", "-F"])
    subprocess.check_output(["ip6tables", "-t", "raw", "-F"])
    try:
        subprocess.check_output(["ip", "link", "del", "dev", "veth1"])
    except:
        pass

def setup_local_veth(conn):
    try:
        teardown_local_veth(conn)
    except:
        pass
    subprocess.check_output(["ip", "link", "add", "veth1", "address", "52:54:00:00:72:55", "type", "veth", "peer", "name", "veth2", "address", "52:54:00:00:55:27"])
    subprocess.check_output(["ip", "link", "set", "veth1", "up"])
    subprocess.check_output(["ip", "link", "set", "veth2", "up"])
    subprocess.check_output(["ip", "address", "add", "fd00::1/64", "dev", "veth1"])
    subprocess.check_output(["ip", "-6", "nei", "add", "fe00::2", "dev", "veth1", "lladdr", "52:54:00:00:55:27", "nud", "permanent"])
    subprocess.check_output(["ip", "-6", "nei", "add", "fe00::3", "dev", "veth2", "lladdr", "52:54:00:00:72:55", "nud", "permanent"])
    subprocess.check_output(["ip", "-6", "route", "add", "fe00::2/128", "dev", "veth1"])
    subprocess.check_output(["ip", "-6", "route", "add", "fe00::3/128", "dev", "veth2"])
    subprocess.check_output(["ip6tables", "-t", "nat", "-A", "PREROUTING", "-d", "fe00::2", "-j", "DNAT", "--to-destination", "fd00::1", "-i", "veth2"])
    subprocess.check_output(["ip6tables", "-t", "nat", "-A", "POSTROUTING", "-d", "fe00::2", "-j", "SNAT", "--to-source", "fe00::3", "-o", "veth1"])
    ConfigurePortod('test-net-sched', """
network {
    managed_device: "veth1"
}
""")
    run_iperf_server('veth-server1', 5201)
    run_iperf_server('veth-server2', 5202)

    cl = None
    deadline = time.time() + 20
    s = server1
    while s and time.time() < deadline:
        if not cl:
            cl = run_iperf_client('cl', s, time=1, wait=0)

        try:
            w = cl.Wait(timeout_s=1)
        except:
            w = None

        if not w:
            continue

        res = int(cl.GetProperty('exit_code'))
        cl.Destroy()
        cl = None
        if res == 0:
            if s == server2:
                s = None
            else:
                s = server2

    if s:
        print "Cannot reliably start local iperf3 servers: %s" % str(s)
        sys.exit(1)

def setup_all_forwarding():
    subprocess.check_output(["sysctl", "-w", "net.ipv6.conf.all.forwarding=1"])

def qdisc_is_classful():
    return qdisc == "htb" or qdisc == "hfsc"

def get_tx_queues_num():
    try:
        queues = os.listdir("/sys/class/net/%s/queues/" % dev)
        return len([q for q in queues if q.startswith("tx")])
    except:
        return 1

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

def run_iperf_client(name, server, wait=None, udp=False, mtn=False, cs=None, reverse=False, cfg={}, **kwargs):
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
    if udp:
        command += " --udp"

    print command

    net = "inherited"
    ip = ""
    if mtn:
        net = "L3 veth"
        ip = "veth fd00::100/128"

    ct = conn.Run(name, command=command, wait=wait, net=net, ip=ip, **cfg)
    if wait:
        if int(ct['exit_code']) != 0:
            print ct['stdout']
            print ct['stderr']

        # check classes of dead container
        if qdisc_is_classful():
            check_porto_net_classes(ct)
        else:
            ExpectProp(ct, "net_class_id", "1:0")
    return ct

def run_iperf_server(name, port):
    command = "bash -c \'while true; do iperf3 --server --bind fd00::1 -p %s ; done\'" % port
    print command
    return conn.Run(name, command=command, weak=False)

def bps(ct):
    ct.WaitContainer(5)
    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bits_per_second'] / 2.**23

def sent_bytes(ct):
    ct.WaitContainer(5)
    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bytes']


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

    a = run_iperf_client("test-net-a", local_server, time=3, wait=20, mtn=True, cfg={"net_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_limit %sM -> " % rate, res
    ExpectRange(res, rate * 0.9, rate * 1.6)

    print "Test net_rx_limit in MTN"

    a = run_iperf_client("test-net-a", local_server, time=3, wait=20, mtn=True, reverse=True, cfg={"net_rx_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_rx_limit %sM -> " % rate, res
    ExpectLe(res, rate * 1.6)

    print "Test both net_limit and net_rx_limit in MTN"

    a = run_iperf_client("test-net-a", local_server, time=3, wait=20, mtn=True, reverse=False, cfg={"net_rx_limit": "default: %sM" % rate, "net_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_limit/net_rx_limit %sM -> " % rate, res
    ExpectLe(res, rate * 1.6)

    a = run_iperf_client("test-net-a", local_server, time=3, wait=20, mtn=True, reverse=True, cfg={"net_rx_limit": "default: %sM" % rate, "net_limit": "default: %sM" % rate})
    res = bps(a)
    print "net_limit/net_rx_limit and reverse %sM -> " % rate, res
    ExpectLe(res, rate * 1.6)

    if qdisc in ["fq_codel", "pfifo_fast"]:
        print "Check tx drops and overlimits"
        a = run_iperf_client("test-net-a", server1, time=5, bandwidth=0, length=1300, wait=20, udp=True, mtn=True, cfg={"net_limit": "default: 100K"})
        ExpectPropGe(a, "net_tx_drops[group default]", 100)
        ExpectPropGe(a, "net_overlimits[group default]", 100)
        ExpectPropLe(a, "net_rx_drops[group default]", 10)
        ExpectPropGe(a, "net_snmp[RetransSegs]", 1)

        Expect(int(a['net_tx_max_speed']) > 50 * 2**20)
        Expect(int(a['net_rx_max_speed']) < 2**20)

        tx_hgram = a['net_tx_speed_hgram'].split(';')
        rx_hgram = a['net_rx_speed_hgram'].split(';')
        ExpectEq(49, len(tx_hgram))
        ExpectEq(49, len(rx_hgram))

        def GetBytes(hgram):
            mbytes = 0
            for kv in hgram:
                bucket, value = [ int(v) for v in kv.split(':') ]
                # 25ms is watchdog period
                mbytes += value * bucket * 0.025
            return mbytes * 2**20

        tx = GetBytes(tx_hgram)
        rx = GetBytes(rx_hgram)
        ExpectRange(tx / float(a['net_tx_bytes[veth]']), 0.75, 1.0)
        ExpectEq(0, rx)

        a.Destroy()

        print "Check rx drops and overlimits"
        a = run_iperf_client("test-net-a", server1, time=5, bandwidth=0, length=1300, wait=20, udp=True, mtn=True, reverse=True, cfg={"net_rx_limit": "default: 100K"})
        ExpectPropGe(a, "net_rx_drops[group default]", 100)
        ExpectPropLe(a, "net_tx_drops[group default]", 10)
        ExpectPropGe(a, "net_snmp[RetransSegs]", 1)

        Expect(int(a['net_rx_max_speed']) > 50 * 2**20)
        Expect(int(a['net_tx_max_speed']) < 2.**20)

        tx_hgram = a['net_tx_speed_hgram'].split(';')
        rx_hgram = a['net_rx_speed_hgram'].split(';')
        ExpectEq(49, len(tx_hgram))
        ExpectEq(49, len(rx_hgram))
        tx = GetBytes(tx_hgram)
        rx = GetBytes(rx_hgram)
        ExpectRange(rx / float(a['net_rx_bytes[veth]']), 0.75, 1.0)
        ExpectEq(0, tx)

        a.Destroy()

def run_bandwidth_sharing_test():
    b = run_iperf_client("test-net-b", server2, time=6, wait=0, cfg={})
    time.sleep(1)
    a = run_iperf_client("test-net-a", server1, time=4, wait=6, cfg={})
    res = bps(a)
    res_b = bps(b)

    b_rate = (6 * res_b - 2 * rate) / 4
    print "net_guarantee 0 and 0 -> %s and %s (%s measured)" % (res, b_rate, res_b)

    # Thresholds rationale:
    # We expect "even" distribution during parallel sending phase, also:
    # 1) Allow up to +- 50% fairness between containers, mind the default rate of 10M
    # 2) Use relative estimation for b container

    ExpectRange(res / b_rate, 0.5, 1.6)

    b = run_iperf_client("test-net-b", server2, time=6, wait=0, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    time.sleep(1)
    a = run_iperf_client("test-net-a", server1, time=4, wait=6, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    res = bps(a)
    res_b = bps(b)
    b_rate = (6 * res_b - 2 * rate) / 4
    print "net_guarantee %sM and %sM -> %s and %s (%s measured)" %(int(rate * 0.1), int(rate * 0.1), res, b_rate, res_b)

    # We demand guarantee and also expect "even distribution"
    # 1) no less than -10% from guarantee, 0.09
    # 2) Use relative estimation for b container
    # 3) Allow up to +- 50% fairness between containers, mind the default rate of 10M

    ExpectLe(0.09, res / rate)
    ExpectLe(0.09, res_b / rate)
    ExpectRange(res / b_rate, 0.5, 1.6)

    b = run_iperf_client("test-net-b", server2, time=6, wait=0, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    time.sleep(1)
    a = run_iperf_client("test-net-a", server1, time=4, wait=6, cfg={"net_guarantee": "default: %sM" % int(rate * 0.9)})
    res = bps(a)
    res_b = bps(b)
    b_rate = (6 * res_b - 2 * rate) / 4
    print "net_guarantee %sM and %sM -> %s and %s measured (%s b_rate)" % (int(rate * 0.9), int(rate * 0.1), res, res_b, b_rate)

    # We demand guarantees to be followed
    # 1) no less than -10% from guarantee, 0.8
    # 2) net-b guarantee keeped

    ExpectLe(0.81, res / rate)
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
    managed_device: "veth1"
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
    managed_device: "veth1"
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
    managed_device: "veth1"
    enable_host_net_classes: false,
    default_qdisc: "default: %s"
}
""" % qdisc)

    run_classless_test()

def run_fq_codel_test():
    set_qdisc("fq_codel")

    ConfigurePortod('test-net-sched', """
network {
    managed_device: "veth1"
    enable_host_net_classes: false,
    default_qdisc: "default: %s"
}
""" % qdisc)

    run_classless_test()

def run_sock_diag_test():
    print "Test non mtn container net stat"

    set_qdisc("fq_codel")

    ConfigurePortod('test-net-sched', """
network {
    enable_host_net_classes: false,
    watchdog_ms: 100,
    sock_diag_update_interval_ms: 500,
    default_qdisc: "default: %s"
}
""" % qdisc)

    meta_a = conn.Create('meta-a', weak=True)
    meta_a.Start()

    root_ct = conn.Find('/')
    loss_bytes_coeff = 4.5 / 5 - 0.01 # (iperf_time - sock_diag_update_interval_ms) / iperf_time - 1%
    iperf_total_sent = 0.

    # check ct stats
    a = run_iperf_client("meta-a/test-net-a", server1, time=5, wait=6)
    tx_bytes = float(a.GetProperty('net_tx_bytes[Uplink]'))
    iperf_sent_bytes = sent_bytes(a)
    ExpectRange(tx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check parent stats
    parent_tx_bytes = float(meta_a.GetProperty('net_tx_bytes[Uplink]'))
    ExpectRange(parent_tx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check root container stats
    iperf_total_sent += iperf_sent_bytes
    root_tx_bytes = float(root_ct.GetProperty('net_tx_bytes[SockDiag]'))
    root_rx_bytes = float(root_ct.GetProperty('net_rx_bytes[SockDiag]'))
    ExpectRange(root_tx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)
    ExpectRange(root_rx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check ct stats
    b = run_iperf_client("meta-a/test-net-a", server1, time=5, wait=6, reverse=True)
    rx_bytes = float(b.GetProperty('net_rx_bytes[Uplink]'))
    iperf_sent_bytes = sent_bytes(b)
    ExpectRange(rx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check parent stats
    parent_rx_bytes = float(meta_a.GetProperty('net_rx_bytes[Uplink]'))
    ExpectRange(parent_rx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check root container stats
    iperf_total_sent += iperf_sent_bytes
    root_rx_bytes = float(root_ct.GetProperty('net_rx_bytes[SockDiag]'))
    root_tx_bytes = float(root_ct.GetProperty('net_tx_bytes[SockDiag]'))
    ExpectRange(root_rx_bytes / iperf_total_sent, loss_bytes_coeff, 1.01)


conn = porto.Connection()

try:
    print "Set net.ipv6.conf.all.forwarding=1"
    setup_all_forwarding()

    print "Setup local veth ifaces veth1/veth2 for iperf3 tests"
    setup_local_veth(conn)

    part = os.environ['PART']
    if part == '1':
        # Tests with net_classes disabled. We do not use net_classes in production

        # common tests
        # run_htb_test(True)
        # run_hfsc_test(True)
        run_pfifo_fast_test()
        run_fq_codel_test()
        run_sock_diag_test()
    elif part == '2':
        # test switching

        # htb -> fq_codel -> htb
        # run_htb_test()
        run_fq_codel_test()
        # run_htb_test()

        # hfsc -> fq_codel -> hfsc
        # run_hfsc_test()
        run_fq_codel_test()
        # run_hfsc_test()

    Expect(0 == int(conn.GetProperty('/', 'porto_stat[errors]')))

finally:
    cts = conn.List()
    if 'test-net-a' in cts:
        conn.Destroy('test-net-a')

    if 'test-net-b' in cts:
        conn.Destroy('test-net-b')

    if 'test-net-s' in cts:
        conn.Destroy('test-net-s')

    print "Cleanup uplink limit"

    teardown_local_veth(conn)
