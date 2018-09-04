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
ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "1")
a = conn.Run("a", net="L3 veth", ip="veth 198.51.100.0", default_gw="veth 198.51.100.1")
ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "2")
b = conn.Run("b", net="L3 veth", ip="veth 198.51.100.0", default_gw="veth 198.51.100.1")
ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "2")
a.Destroy()
b.Destroy()


# net=none
a = conn.Run("a", net="none", command="sleep 60")
b = conn.Run("b", net="none", command="sleep 60")
ExpectEq(conn.GetProperty("/", "porto_stat[networks]"), "3")
a.Destroy()
b.Destroy()
