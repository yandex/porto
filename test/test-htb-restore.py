import porto
from test_common import *
import os
import re
import subprocess

def has_class(link, class_id):
    (major, minor) = class_id.split(':')
    expr = 'class (htb|hfsc) %s\:%s' % (major, minor)
    out = subprocess.check_output(['tc', 'class', 'show', 'dev', link])
    return re.search(expr, out)


def get_parent_id(link, class_id):
    m = re.search(
        'class (htb|hfsc) %s parent ([0-9a-f]+:[0-9a-f]+)' % class_id,
        subprocess.check_output([
            'tc', 'class', 'show', 'dev', link
        ])
    )

    assert m, "Cannot get valid tclass parent"
    return m.groups()[1]


def get_cs_ids(conn, ct):
    cs_ids = [""] * 8
    cs_leaf_ids = [""] * 8

    for i in xrange(0, 8):
        cs_ids[i] = conn.GetProperty(ct, "net_class_id[CS%s]" % i)
        cs_leaf_ids[i] = conn.GetProperty(ct, "net_class_id[Leaf CS%s]" % i)

    return cs_ids, cs_leaf_ids


conn = porto.Connection(timeout=5)

r = conn.Create("a")

try:
    ConfigurePortod("htb-restore", """
network {
    device_qdisc: "default: hfsc",
    enable_host_net_classes: true,
    managed_ip6tnl: true,
    enforce_unmanaged_defaults: true
}
""")

    r2 = conn.Create("a/b")
    rr = conn.Create("a/b/c")
    rr.SetProperty("command", "/bin/sleep infinity")
    rr.SetProperty("net_limit", "default: 0")
    rr.SetProperty("net_guarantee", "default: 0")
    rr.Start()

    _, a_leaf_ids = get_cs_ids(conn, "a/b/c")
    root_ids, _ = get_cs_ids(conn, "/")

    managed_links = [link for link in os.listdir('/sys/class/net') if has_class(link, root_ids[0])]
    assert managed_links

    for link in managed_links:
        parent_id = get_parent_id(link, a_leaf_ids[0])
        assert parent_id
        assert parent_id != "root"

    ConfigurePortod("htb-restore", """
network {
    device_qdisc: "default: htb",
    enable_host_net_classes: true,
    managed_ip6tnl: true,
    enforce_unmanaged_defaults: true
}
""")

    assert conn.GetProperty("a/b/c", "state") == "running"

    for link in managed_links:
        for i in xrange(0, 8):
            parent_id = get_parent_id(link, a_leaf_ids[i])
            assert parent_id
            assert parent_id != "root"

finally:
    try:
        r.Destroy()
        ConfigurePortod("htb-restore", "")
    except:
        pass
