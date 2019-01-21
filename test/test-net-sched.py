import porto
import json
import time
import os
import sys
from test_common import *

remote_host = os.environ.get('PORTO_IPERF3_SERVER', None)

if not remote_host:
    print "No remote host specified, skipping test"
    sys.exit(0)

server1=(remote_host, 5201)
server2=(remote_host, 5202)


def run_iperf(name, server, wait=None, cs=None, cfg={}, **kwargs):
    command="iperf3 --client " + server[0]
    command += " --port " + str(server[1])

    if cs is not None:
        command += " --tos " + str(hex((cs & 7) << 6))

    for k,v in kwargs.items():
        command += " --" + k.replace('_', '-')
        if v is not None:
            command += "=" + str(v)

    command += " --json"

    print command
    return conn.Run(name, command=command, wait=wait, **cfg)


def bps(ct):
    ct.WaitContainer(5)
    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bits_per_second'] / 2.**23


def run_test(rate=10):
    a = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: 0"})
    res = bps(a)
    print "net_limit inf -> ", res
    ExpectRange(res, rate * 0.8, rate * 1.33)

    a = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: %sM" % int(rate * 0.1)})
    res = bps(a)
    print "net_limit %sM -> " % int(rate * 0.1), res
    ExpectRange(res, rate * 0.05, rate * 0.2)

    b = run_iperf("test-net-b", server2, time=5, wait=0, cfg={})
    a = run_iperf("test-net-a", server1, time=3, wait=5, cfg={})
    res = bps(a)
    res_b = bps(b)

    b_rate = (5 * res_b - 2 * rate) / 3
    print "net_guarantee 0 and 0 -> %s and %s (%s measured)" % (res, b_rate, res_b)

    # Thresholds rationale:
    # We expect "even" distribution during parallel sending phase, also:
    # 1) Allow up to +- 50% fairness between containers, mind the default rate of 10M
    # 2) Use relative estimation for b container

    ExpectRange(res / b_rate, 0.5, 1.5)

    b = run_iperf("test-net-b", server2, time=5, wait=0, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    a = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
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

    b = run_iperf("test-net-b", server2, time=5, wait=0, cfg={"net_guarantee": "default: %sM" % int(rate * 0.1)})
    a = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_guarantee": "default: %sM" % int(rate * 0.9)})
    res = bps(a)
    res_b = bps(b)
    b_rate = (5 * res_b - 2 * rate) / 3
    print "net_guarantee %sM and %sM -> %s and %s measured (%s b_rate)" % (int(rate * 0.9), int(rate * 0.1), res, res_b, b_rate)

    # We demand guarantees to be followed
    # 1) no less than -10% from guarantee, 0.8
    # 2) net-b guarantee keeped

    ExpectLe(0.85, res / rate)
    ExpectLe(0.09, res_b / rate)

conn = porto.Connection()
rate = 10

try:
    print "Test HTB scheduler"
    print "Setup uplink limit %sM" % rate

    ConfigurePortod('test-net-sched', """
network {
    device_ceil: "default: %sM
    device_rate: "default: %sM"
    device_qdisc: "default: htb"
}
""" % (rate, rate))

    run_test(rate)

    print "Test HFSC scheduler"
    print "Setup uplink limit %sM" % rate

    ConfigurePortod('test-net-sched', """
network {
    device_ceil: "default: %sM"
    device_qdisc: "default: hfsc"
}
""" % rate)

    run_test(rate)

finally:
    cts = conn.List()
    if 'test-net-a' in cts:
        conn.Destroy('test-net-a')

    if 'test-net-b' in cts:
        conn.Destroy('test-net-b')

    print "Cleanup uplink limit"
    ConfigurePortod('test-net-sched', "")
