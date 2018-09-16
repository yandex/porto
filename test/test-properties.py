import porto
import datetime
from test_common import *

size_test =  [
        ("1234567", 1234567),
        ("0", 0),
]

path_test = [
    "test",
    "/a/b/c",
]

bool_test = [
    True,
    False,
]

cpu_test = [
    ("0c", 0),
    ("1c", 1),
    ("3.14c", 3.14),
]

weight_test = [
    ("1", 1),
    ("10", 10),
    ("0.01", 0.01),
    ("100", 100),
]

map_test = [
    ("", {}),
    ("default: 1", {'map': [{'key': "default", 'val': 1}]}),
    ("a: 0; b: 1000", {'map': [{'key': "a", 'val': 0}, {'key': "b", 'val': 1000}]}),
]

tests = {
"virt_mode": [
    "app",
    "os",
    "job",
    "host",
],
"enable_porto": [
    (True, "true"),
    (False, "false"),
    "isolate",
    "read-only",
    "read-isolate",
    "child-only",
],
"weak": bool_test,
"command": [
    "",
    "sleep 10",
    "/sbin/init",
],
"core_command": [
    "",
    "foo bar baz",
],
"user": [
    "root",
    "porto-alice",
],
"group": [
    "root",
    "porto-alice",
],
"owner_user": [
    "root",
    "porto-alice",
],
"owner_group": [
    "root",
    "porto-alice",
],
"env": [
    ("TEST=123", {'var': [ {'name': 'TEST', 'value': '123'} ] } ),
],
"porto_namespace": [
    ""
    "a",
    "a/b/c"
],
"place": [
    ("/place;***", {'cfg': [ {'place': '/place'}, {'place': '***'} ] } ),
    ("", {}),
    ("/place;slow=/hdd;fast=/ssd", {'cfg': [
        {'place': '/place'},
        {'place': '/hdd', 'alias': 'slow'},
        {'place': '/ssd', 'alias': 'fast'}
    ] }),
],
"place_limit": [
    ("", {}),
    ("default: 100", {"map": [ { "key": "default", "val": 100 } ] }),
],
"root": [
    "/",
    "/foo/bar",
],
"root_readonly": [
    (False, False),
    (True, True),
],
"cwd": [
    "/",
    "/tmp",
],
"stdin_path": path_test,
"stdout_path": path_test,
"stderr_path": path_test,
"stdout_limit": size_test,
"umask": [
    ("0755", 0o755),
    ("0700", 0o700),
],

"controllers": [
    ("", {}),
    ("freezer;memory", {'controller': ['freezer', 'memory']}),
],

"thread_limit": size_test,

"sysctl": [
    ("", {}),
    ("net.core.somaxconn: 1024; net.unix.max_dgram_qlen: 20",
        {'map': [{'val': u'1024', 'key': u'net.core.somaxconn'},
            {'val': u'20', 'key': u'net.unix.max_dgram_qlen'}]}),
],

"memory_guarantee": size_test,
"memory_limit": size_test,
"anon_limit": size_test,
"dirty_limit": size_test,
"hugetlb_limit": size_test,
"recharge_on_pgfault": bool_test,
"pressurize_on_death": bool_test,
"anon_only":  bool_test,
"oom_is_fatal": bool_test,
"oom_score_adj": [
    ("0", 0),
    ("1000", 1000),
    ("-1000", -1000),
],
"cpu_policy": [
    "normal",
    "rt",
    "idle",
    "batch",
    "high",
],

"cpu_guarantee": cpu_test,
"cpu_limit": cpu_test,
"cpu_period": [],
"cpu_weight": weight_test,
"cpu_set": [
    ("0-1", {'policy': 'set', 'count': 2, 'list': '0-1', 'cpu': [0, 1]}),
    ("node 0", {'policy': 'node', 'arg': 0}),
],
"io_policy": [
    "none",
    "normal",
    "batch",
    "idle",
    "rt",
],
"io_weight": weight_test,
"io_limit": map_test,
"io_ops_limit": map_test,
"net_guarantee": map_test,
"net_limit": map_test,
"net_rx_limit": map_test,
"respawn": bool_test,
"max_respawns": size_test,
"respawn_delay": size_test,
"isolate": bool_test,
"private": [
    ("", ""),
    "a b c !@#$%^*()_-+'''::::><><?SDS?/\???\\",
],
"labels": [
    ("", {}),
    ("ABC.abc: abc; TEST.test: 1", {'map': [{'key': "ABC.abc", 'val': "abc"}, {'key': "TEST.test", 'val': "1"}]}),
],
"aging_time": size_test,
"hostname": [
    "",
    "foo",
],
"bind": [
    ("", {}),
    ("/a /b ro", {'bind': [{'source': '/a', 'target': '/b', 'flag': [u'ro']}]}),
    ("/a /b ;c d ", {'bind': [{'source': '/a', 'target': '/b'}, {'source': 'c', 'target': 'd'}]}),
],
"symlink": [
    ("", {}),
    ("a: b; ", {'map': [{'key': 'a', 'val': 'b'}]}),
    ("a: b; c: d; ", {'map': [{'key': 'a', 'val': 'b'}, {'key': 'c', 'val': 'd'}]}),
],
"net": [
    ("inherited", {'cfg': [{'opt': u'inherited'}], 'inherited': True}),
    ("none",      {'cfg': [{'opt': u'none'}], 'inherited': False}),
    ("tap tun",   {'cfg': [{'opt': u'tap', 'arg': [u'tun']}], 'inherited': True}),
    ("L3 veth",   {'cfg': [{'opt': u'L3', 'arg': [u'veth']}], 'inherited': False}),
    ("L3 veth;ipip6 tun 1::1 2::2", {'cfg': [{'opt': u'L3', 'arg': [u'veth']}, {'opt': u'ipip6', 'arg': [u'tun', u'1::1', u'2::2']}], 'inherited': False}),
],
"ip": [
    ("", {}),
    ("eth0 192.168.1.1", {'cfg': [{'dev': 'eth0', 'ip': '192.168.1.1'}]}),
    ("eth0 192.168.1.1;eth0 1::1", {'cfg': [{'dev': 'eth0', 'ip': '192.168.1.1'}, {'dev': 'eth0', 'ip': '1::1'}]}),
    ("eth0 192.168.1.1/24", {'cfg': [{'dev': 'eth0', 'ip': '192.168.1.1/24'}]}),
],
"default_gw": [
    ("", {}),
    ("eth0 192.168.1.1", {'cfg': [{'dev': 'eth0', 'ip': '192.168.1.1'}]}),
    ("eth0 192.168.1.1;eth0 1::1", {'cfg': [{'dev': 'eth0', 'ip': '192.168.1.1'}, {'dev': 'eth0', 'ip': '1::1'}]}),
    ("eth0 192.168.1.1/24", {'cfg': [{'dev': 'eth0', 'ip': '192.168.1.1/24'}]}),
],
"ip_limit": [
    ("any", {'policy': 'any'}),
    ("none", {'policy': 'none'}),
    ("192.168.1.1", {'policy': 'some', 'ip': ['192.168.1.1']}),
    ("192.168.1.1/24", {'policy': 'some', 'ip': ['192.168.1.1/24']}),
    ("1::1", {'policy': 'some', 'ip': ['1::1']}),
    ("1::1/127", {'policy': 'some', 'ip': ['1::1/127']}),
],
"net_tos": [
    "CS1",
    "CS7",
],
"resolv_conf": [
    "inherited",
    "keep",
    "nameserver 8.8.8.8",
],
"etc_hosts": [
    "",
    "127.0.0.1 localhost",
],
"devices": [
    ("", {}),
    ("/dev/null rm /dev/null 0666 root root; ",
        {'device': [{'device': u'/dev/null', 'access': u'rm'}]}),
],
"capabilities": [
    ("", {'hex': '0000000000000000'}),
    ("KILL;SYS_PTRACE", {'cap': ['KILL', 'SYS_PTRACE'], 'hex': '0000000000080020'}),
],
"capabilities_ambient": [
    ("", {'hex': '0000000000000000'}),
    ("KILL;SYS_PTRACE", {'cap': ['KILL', 'SYS_PTRACE'], 'hex': '0000000000080020'}),
],
}


ro_tests = {
"taint": [],

"ulimit": [],

"task_cred": [],
"owner_cred": [],

"root_path": [],

"anon_usage": [],
"memory_usage": [],
"memory_reclaimed": [],
"anon_limit_total": [],
"memory_limit_total": [],
"memory_guarantee_total": [],
"anon_max_usage": [],
"cache_usage": [],
"hugetlb_usage": [],
"minor_faults": [],
"major_faults": [],
"virtual_memory": [],

"cpu_set_affinity": [],
"cpu_guarantee_total": [],
"cpu_limit_total": [],
"cpu_usage": [],
"cpu_usage_system": [],
"cpu_wait": [],
"cpu_throttled": [],
"capabilities_allowed": [],
"capabilities_ambient_allowed": [],

"respawn_count": [],
"id": [],
"level": [],
"absolute_name":[],
"absolute_namespace":[],
"state": [],
"oom_kills": [],
"oom_kills_total": [],
"parent": [],
"root_pid": [],

"stdout_offset": [],
"stderr_offset": [],

"net_class_id": [],
"net_bytes": [],
"net_packets": [],
"net_drops": [],
"net_overlimits": [],
"net_rx_bytes": [],
"net_rx_packets": [],
"net_rx_drops": [],
"net_tx_bytes": [],
"net_tx_packets": [],
"net_tx_drops": [],
"io_read": [],
"io_write": [],
"io_ops": [],
"io_time": [],
"time": [],
"creation_time": [],
"start_time": [],
"change_time": [],
"cgroups": [],
"process_count": [],
"thread_count": [],
"place_usage": [],
"volumes_owned": [],
"volumes_linked": [],
"volumes_required": [],
}

dead_tests = {
"exit_status": [],
"exit_code": [],
"death_time": [],
"core_dumped": [],
"oom_killed": [],
}

skip_tests = {
"max_rss":[],
"bind_dns": [],
"stdout": [],
"stderr": [],
"porto_stat": [],
"start_error": [],
"command_argv": [],
}

ConfigurePortod('test-coredump', """
core {
    enable: true
    default_pattern: "/tmp/core"
}
""")

conn = porto.Connection()

ct_name = "a"

def get_old(name):
    return conn.GetProperty(ct_name, name)

def get_new(name):
    return conn.Call('GetContainer', name=[ct_name], property=[name])['container'][0].get(name)

def get_sub(name, field, key, val):
    for sub in get_new(name)[field]:
        if sub[key] == val:
            return sub
    return None

def set_old(name, value):
    conn.SetProperty(ct_name, name, value)

def set_new(name, value):
    conn.Call('SetContainer', container={'name': ct_name, name: value})

def format_time(ts):
    return datetime.datetime.fromtimestamp(int(ts)).isoformat(' ')

for name, cases in tests.iteritems():
    print " - ", name

    ct = conn.Create(ct_name, weak=True)

    init_old = get_old(name)
    init_new = get_new(name)

    for case in cases:
        if isinstance(case, tuple):
            old = case[0]
            new = case[1]
        else:
            old = new = case

        # print "   ", old, new

        set_old(name, old)
        ExpectEq(get_old(name), old)
        ExpectEq(get_new(name), new)

        if name not in ['labels', 'devices']:
            set_old(name, init_old)
            ExpectEq(get_old(name), init_old)
            ExpectEq(get_new(name), init_new)

        set_new(name, new)
        ExpectEq(get_old(name), old)
        ExpectEq(get_new(name), new)

        if name not in ['labels', 'devices']:
            set_new(name, init_new)
            ExpectEq(get_old(name), init_old)
            ExpectEq(get_new(name), init_new)

    conn.Destroy(ct_name)


ct = conn.Run(ct_name)

for name, cases in ro_tests.iteritems():
    print " - ", name
    init_old = get_old(name)
    ExpectNe(init_old, None)
    init_new = get_new(name)
    ExpectNe(init_new, None)
    # print "   ", init_old, init_new

ct.Destroy()

ct = conn.Run(ct_name, wait=1, command="true")

for name, cases in dead_tests.iteritems():
    print " - ", name
    init_old = get_old(name)
    ExpectNe(init_old, None)
    init_new = get_new(name)
    ExpectNe(init_new, None)
    # print "   ", init_old, init_new

for name in ['creation_time', 'change_time', 'start_time', 'death_time']:
    print " - ", name + '[raw]'
    ExpectEq(get_old(name), format_time(get_old(name + '[raw]')))
    ExpectEq(int(get_old(name + '[raw]')), get_new(name))

ct.Destroy()

ct = conn.Run(ct_name)

print " -  ulimit core"
set_old('ulimit[core]', 'unlimited')
ExpectEq(get_old('ulimit[core]'), 'core: unlimited unlimited')
set_old('ulimit[core]', '0 unlimited')
ExpectEq(get_sub('ulimit', 'ulimit', 'type', 'core'), {'type': 'core', 'soft': 0})
set_new('ulimit', {'merge': True, 'ulimit': [{'type': 'core', 'unlimited': True}]})
ExpectEq(get_sub('ulimit', 'ulimit', 'type', 'core'), {'type': 'core', 'unlimited': True})
ExpectEq(get_old('ulimit[core]'), 'core: unlimited unlimited')

ct.Destroy()


print " -  command_argv"

ct = conn.Create(ct_name, weak=True);

set_old('command', 'a b c')
ExpectEq(get_old('command'), 'a b c')
ExpectEq(get_new('command'), 'a b c')
ExpectEq(get_old('command_argv'), '')
ExpectEq(get_new('command_argv'), {})

set_old('command_argv', "a\t'b\tc'\td'd")
ExpectEq(get_old('command'), "'a' ''\\''b' 'c'\\''' 'd'\\''d' ")
ExpectEq(get_new('command'), "'a' ''\\''b' 'c'\\''' 'd'\\''d' ")
ExpectEq(get_old('command_argv'), "a\t'b\tc'\td'd")
ExpectEq(get_new('command_argv'), {'argv': ["a", "'b", "c'", "d'd"]})

ExpectEq(get_old('command_argv[3]'), "d'd")
set_old('command_argv[4]', 'x x')
ExpectEq(get_old('command'), "'a' ''\\''b' 'c'\\''' 'd'\\''d' 'x x' ")
ExpectEq(get_new('command'), "'a' ''\\''b' 'c'\\''' 'd'\\''d' 'x x' ")
ExpectEq(get_old('command_argv'), "a\t'b\tc'\td'd\tx x")
ExpectEq(get_new('command_argv'), {'argv': ["a", "'b", "c'", "d'd", "x x"]})

set_new('command', 'xxx')
ExpectEq(get_old('command'), 'xxx')
ExpectEq(get_new('command'), 'xxx')
ExpectEq(get_old('command_argv'), '')
ExpectEq(get_new('command_argv'), {})

set_new('command_argv', {'argv': ["a", "'b", "c'", "d'd"]})
ExpectEq(get_old('command'), "'a' ''\\''b' 'c'\\''' 'd'\\''d' ")
ExpectEq(get_new('command'), "'a' ''\\''b' 'c'\\''' 'd'\\''d' ")
ExpectEq(get_old('command_argv'), "a\t'b\tc'\td'd")
ExpectEq(get_new('command_argv'), {'argv': ["a", "'b", "c'", "d'd"]})

ct.Destroy()



for name in conn.Plist():
    assert name in  tests.keys() + ro_tests.keys() + dead_tests.keys() + skip_tests.keys(), "property {} is not tested".format(name)

ConfigurePortod('test-coredump', "")

