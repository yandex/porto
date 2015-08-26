By default container shares network with host.
This might be changed via net property, which isolates container from the host network.

# Properties

* hostname - container hostname (man 2 gethostname)
* net - isolate container network, synax: none | inherited | host [interface] | macvlan <master> <name> [type] [mtu] [hw] | veth <name> <bridge> [mtu] [hw] | netns <name>
  - by default network is inherited from parent container (i.e. host)
  - none - no networking, except for loopback device
  - host - move host interface into container
  - macvlan - crate macvlan from host interface <master>
  - veth - create veth pair and add one end into <bridge> and another end into container
  - netns - use network namespace created via ip utility

# Data

* net\_bytes - number of TX bytes, syntax: <iface>: <counter>; ...
* net\_drops - number of dropped TX packets
* net\_overlimits - number of TX overlimits
* net\_packets - number of TX packets

# Examples

```
portoctl exec trusty command='ip l' net='macvlan eth0 eth0'
```
