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
a.Destroy()
a = conn.Run("a", net="L3 veth", ip="veth 2001:db8::", default_gw="veth 2001:db8::1")
a.Destroy()


# ip migration
ConfigurePortod('test-net', """
network {
    watchdog_ms: 100,
    sock_diag_update_interval_ms: 500,
""")

ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "1")
a = conn.Run("a", net="L3 veth", ip="veth 198.51.100.0", default_gw="veth 198.51.100.1")
b = conn.Run("a/b", command="ping -c1 -s1 localhost", wait=5)
time.sleep(2)
assert len(a['net_netstat'].split(';')) > 100
assert a['net_netstat[OutOctets]'] == '58'
assert a['net_netstat[TCPBacklogDrop]'] == '0'

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
