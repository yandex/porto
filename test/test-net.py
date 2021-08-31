from test_common import *
import porto

conn = porto.Connection()

ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "1")

# inherit over isolate=false
a = conn.Run("a", net="none")
b = conn.Run("a/b", isolate="false")
c = conn.Run("a/b/c")
a.Destroy()


# unknown gateway
ExpectEq(Catch(conn.Run, "a", net="L3 veth", ip="veth 198.51.100.0"), porto.exceptions.InvalidNetworkAddress)
ExpectEq(Catch(conn.Run, "a", net="L3 veth", ip="veth 2001:db8::"), porto.exceptions.InvalidNetworkAddress)


# known gateway
a = conn.Run("a", net="L3 veth", ip="veth 198.51.100.0", default_gw="veth 198.51.100.1")

# '..' not allowed in net namespace path
ExpectEq(Catch(conn.Run, "a/b", net="netns ../../proc/1/ns/net"), porto.exceptions.Permission)

a.Destroy()
a = conn.Run("a", wait=1, command="ip -6 r", net="L3 veth", ip="veth 2001:db8::", default_gw="veth 2001:db8::1")
default_routes = a['stdout'].split('\n')
a.Destroy()


# test extra_routes
ConfigurePortod('test-net', """
network {
    extra_routes {
        dst: "default"
        mtu: 1450
        advmss: 1390
    }
    extra_routes {
        dst: "64:ff9b::/96"
        mtu: 1450
        advmss: 1390
    }
    extra_routes {
        dst: "2a02:6b8::/32"
        mtu: 8910
    }
    extra_routes {
        dst: "2620:10f:d000::/44"
        mtu: 8910
    }
""")

extra_routes = ['64:ff9b::/96 via 2001:db8::1 dev veth  proto static  metric 1024  mtu 1450 advmss 1390 pref medium',
                '2620:10f:d000::/44 via 2001:db8::1 dev veth  proto static  metric 1024  mtu 8910 pref medium',
                '2a02:6b8::/32 via 2001:db8::1 dev veth  proto static  metric 1024  mtu 8910 pref medium',
                'default via 2001:db8::1 dev veth  proto static  metric 1024  mtu 1450 advmss 1390 pref medium']

def TestExtraRoutes(enable):
    # extra routes enabled by default
    a = conn.Run("a", net="L3 {}veth".format("extra_routes " if enable else ""), ip="veth 2001:db8::", default_gw="veth 2001:db8::1")
    ab = conn.Run("a/b", wait=1, command="ip -6 r")
    routes = ab['stdout'].replace('\t', ' ').split('\n')

    for route in extra_routes:
        if route not in routes:
            raise BaseException("Extra route not found {}".format(route))

    a.Destroy()

TestExtraRoutes(enable=True)
TestExtraRoutes(enable=False)


# ip migration
ConfigurePortod('test-net', """
network {
    watchdog_ms: 100,
    sock_diag_update_interval_ms: 500
}
""")

ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "1")
a = conn.Run("a", net="L3 veth", ip="veth 198.51.100.0", default_gw="veth 198.51.100.1")
b = conn.Run("a/b", command="ping -c1 -s1 localhost", wait=5)
time.sleep(2)
assert len(a['net_netstat'].split(';')) > 100
assert a['net_netstat[OutOctets]'] == '58', "OutOctets value: {}".format(a['net_netstat[OutOctets]'])
assert a['net_netstat[TCPBacklogDrop]'] == '0'
assert a['net_snmp[RetransSegs]'] == '0'
assert a['net_snmp[InErrors]'] == '0'

ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "2")
b = conn.Run("b", net="L3 veth", ip="veth 198.51.100.0", default_gw="veth 198.51.100.1")
ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "2")
a.Destroy()
b.Destroy()

ConfigurePortod('test-net', '')


# net=none
a = conn.Run("a", net="none", command="sleep 60")
b = conn.Run("b", net="none", command="sleep 60")
ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "3")
a.Destroy()
b.Destroy()
