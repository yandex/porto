import porto
import json
import time
from test_common import *


server1=("kernel1.search.yandex.net", 5201)
server2=("kernel1.search.yandex.net", 5202)
server3=("kernel1.search.yandex.net", 5203)


conn = porto.Connection()


print "Setup uplink limit 10M"
ConfigurePortod('test-net-sched', """
network {
    device_ceil: "default: 10M"
}
""")



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
    ct = conn.Run(name, command=command, wait=wait, **cfg)
    if wait == 0:
        return None

    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bits_per_second'] / 2.**23


res = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: 0"})
print "net_limit inf -> ", res
ExpectRange(res, 8, 12)


res = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: 1M"})
print "net_limit 1M -> ", res
ExpectRange(res, 0.5, 1.5)


run_iperf("test-net-b", server2, time=5, wait=0, cfg={})
res = run_iperf("test-net-a", server1, time=3, wait=5, cfg={})
conn.Destroy("test-net-b")
print "net_guarantee 0 and 0 -> ", res
ExpectRange(res, 4, 6)


run_iperf("test-net-b", server2, time=5, wait=0, cfg={"net_guarantee": "default: 1M"})
res = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_guarantee": "default: 1M"})
conn.Destroy("test-net-b")
print "net_guarantee 1M and 1M -> ", res
ExpectRange(res, 4, 6)


run_iperf("test-net-b", server2, time=5, wait=0, cfg={"net_guarantee": "default: 2M"})
res = run_iperf("test-net-a", server1, time=3, wait=5, cfg={"net_guarantee": "default: 3M"})
conn.Destroy("test-net-b")
print "net_guarantee 2M and 3M -> ", res
ExpectRange(res, 5, 7)


print "Remove uplink limit"
ConfigurePortod('test-net-sched', "")
