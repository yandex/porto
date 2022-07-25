import os
import subprocess
import porto
import shutil
from test_common import *

AsRoot()

PREV_VERSION = "5.0.11"

TMPDIR = "/tmp/test-release-upgrade"
prev_portod = TMPDIR + "/old/usr/sbin/portod"

try:
    os.mkdir(TMPDIR)
except BaseException as e:
    shutil.rmtree(TMPDIR)
    os.mkdir(TMPDIR)

def DumpLegacyRt(r):
    res =  (r.GetProperty("cpu.cfs_quota_us"),
            r.GetProperty("cpu.cfs_period_us"),
            r.GetProperty("cpu.shares"))

    try:
        res = res + (r.GetProperty("cpu.cfs_reserve_shares"), r.GetProperty("cpu.cfs_reserve_us"),)
    except:
        pass

    try:
        res = res + (r.GetProperty("cpu.smart"), )
    except:
        pass

    return res

def CheckRt(r):
    pid = r.GetProperty("root_pid")
    task_stat = open("/proc/{}/stat".format(pid), "r").read().split()
    (nice, prio, policy) = [task_stat[i] for i in [18, 39, 40]]
    assert nice == "-20"
    assert prio == "10"
    assert policy == "2"

#FIXME: remove it in the future, use capabilities from snapshot
def CheckCaps(r):
    root_path = r.GetProperty("root_path")

    app_caps = "CHOWN;DAC_OVERRIDE;FOWNER;FSETID;KILL;SETGID;SETUID;SETPCAP;"
    app_caps += "LINUX_IMMUTABLE;NET_BIND_SERVICE;NET_ADMIN;NET_RAW;IPC_LOCK;"
    app_caps += "SYS_CHROOT;SYS_PTRACE;SYS_ADMIN;"

    # Host-chroot containers still have host bounding set
    app_caps += "" if root_path != '/' else "SYS_BOOT;"
    app_caps += "SYS_NICE;SYS_RESOURCE;MKNOD;AUDIT_WRITE;SETFCAP"

    os_caps = "CHOWN;DAC_OVERRIDE;FOWNER;FSETID;KILL;SETGID;SETUID;SETPCAP;"
    os_caps += "NET_BIND_SERVICE;NET_ADMIN;NET_RAW;IPC_LOCK;SYS_CHROOT;SYS_PTRACE;"

    # Host-chroot containers still have host bounding set
    os_caps += "" if root_path != '/' else "SYS_BOOT;"
    os_caps += "MKNOD;AUDIT_WRITE;SETFCAP"

    legacy_os_caps = "AUDIT_WRITE; CHOWN; DAC_OVERRIDE; FOWNER; FSETID; IPC_LOCK; KILL; MKNOD; NET_ADMIN; NET_BIND_SERVICE; NET_RAW; SETGID; SETUID; SYS_CHROOT; SYS_PTRACE; SYS_RESOURCE"

    if r.GetProperty("virt_mode") == "app":
        caps = app_caps
    elif r.GetProperty("virt_mode") == "os":
        caps = os_caps
    else:
        raise AssertionError("Found unexpected virt_mode value")

    ExpectProp(r, "capabilities", caps)

#FIXME: remove it in ther future
def PropTrim(prop):
    if not (type(prop) is str or type(prop) is unicode):
        return prop
    prop = str(prop.replace("; ", ";"))
    if len(prop) > 0 and prop[-1] == ';':
        prop = prop[:-1]
    return prop

def SetProps(r, props):
    for p in props:
        r.SetProperty(p[0], p[1])

def VerifyProps(r, props):
    for p in props:
        value = r.GetProperty(p[0])
        try:
            assert PropTrim(p[1]) == PropTrim(value)
        except AssertionError as e:
            print "{} prop value <{}> != <{}>".format(p[0], p[1], value)
            raise e

def SnapshotProps(r):
    #FIXME: add controllers, cpu_set, owner_group, owner_user, umask later
    props = [ "aging_time",
              # "anon_limit", FIXME
              "bind",
              #"capabilities", #FIXME enable later, os: "<set1>" -> "<set2>" ,
                               #app: "" -> "<not empty>"
              "command", "cpu_guarantee",
              # "cpu_limit",
              "cpu_policy",
              "cwd", "devices", "enable_porto", "env",
              #"group",
              "hostname",
              # "io_limit", "io_ops_limit", FIXME "0" -> ""
              # "io_policy",
              "ip",
              "isolate",
              # "max_respawns",
              "memory_guarantee",
              # "memory_limit",
              "net",
              #"net_guarantee", #FIXME enable later, "default:0" -> ""
              #"net_limit", #FIXME enable later, "default:0" -> ""
              "porto_namespace",
              "private", "recharge_on_pgfault",
              # "resolv_conf",
              "respawn", "root", "root_readonly",
              #"stderr_path", #FIXME enable later, "/dev/null" -> ""
              #"stdout_path", #FIXME enable later, "/dev/null" -> ""
              "stdin_path", "stdout_limit", "ulimit",
              #"user", FIXME virt_mode=os
              "virt_mode", "weak" ]
    d = dict()
    for p in props:
        d[p] = r.GetProperty(p)

    return d

def VerifySnapshot(r, props):
    props2 = SnapshotProps(r)
    for i in props:
        assert PropTrim(props[i]) == PropTrim(props2[i]), "{} property {} should be {} not {}".format(r, i, props[i], props2[i])


def CheckNetworkProblems():
    conn = porto.Connection(timeout=3)
    a = conn.Call('GetSystem')
    conn.Get(['/'], ['net_bytes'], sync=True)
    b = conn.Call('GetSystem')
    assert a.get('network_problems') == b.get('network_problems'), "Network problems detected"


c = porto.Connection(timeout=3)

#Check  working with older version

print "Checking upgrade from", PREV_VERSION

ver, rev = c.Version()
ExpectNe(ver, PREV_VERSION)

cwd=os.path.abspath(os.getcwd())

os.chdir(TMPDIR)
subprocess.call(["apt-get", "update"])
download = subprocess.check_output(["apt-get", "--force-yes", "download", "yandex-porto=" + PREV_VERSION])

print "Package successfully downloaded"

StopPortod()

downloads = download.split('\n')[0].split()
pktname = downloads[-4] + "_" + downloads[-3] + "_amd64.deb"

os.mkdir("old")
subprocess.check_call(["dpkg", "-x", pktname, "old"])
os.unlink(pktname)

os.symlink(TMPDIR + "/old/usr/lib/porto/portoinit", "old/usr/sbin/portoinit")

os.chdir(cwd)

print " - start previous version"

subprocess.check_call([prev_portod, "start"])

ver, rev = c.Version()
ExpectEq(ver, PREV_VERSION)
CheckNetworkProblems()

AsAlice()

c = porto.Connection(timeout=3)

c.Create("test")
c.SetProperty("test", "command", "sleep 5")
c.Start("test")

c.Create("test2")
c.SetProperty("test2", "command", "bash -c 'sleep 20 && echo 456'")
c.Start("test2")

parent_knobs = [
    ("private", "parent"),
    ("respawn", False),
    ("ulimit", "data: 16000000 32000000; memlock: 4096 4096; nofile: 100 200; nproc: 500 1000; "),
    ("isolate", True),
    ("env", "CONTAINER=porto;PARENT=1")
]

AsRoot()
ConfigurePortod('test-prev_release_upgrade', """
container {
    rt_priority: 10
}""")
AsAlice()

try:
    r = c.Create("parent_app")
    SetProps(r, parent_knobs)
    VerifyProps(r, parent_knobs)
    snap_parent_app = SnapshotProps(r)

    app_knobs = [
        ("cpu_limit", "1c"),
        ("private", "parent_app"),
        ("respawn", False),
        ("cpu_policy", "normal"),
        ("memory_guarantee", "16384000"),
        ("command", "sleep 20"),
        ("memory_limit", "512000000"),
        ("cwd", portosrc),
        ("net_limit", "default: 0"),
        ("cpu_guarantee", "0.01c"),
        ("ulimit", "data: 16000000 32000000; memlock: 4096 4096; nofile: 100 200; nproc: 500 1000; "),
        ("io_limit", "300000"),
        ("isolate", False),
        ("env", "CONTAINER=porto;PARENT=1;TAG=mytag mytag2 mytag3")
    ]

    r = c.Create("parent_app/app")
    SetProps(r, app_knobs)
    VerifyProps(r, app_knobs)
    snap_app = SnapshotProps(r)
    r.Start()

    v = c.CreateVolume(None, layers=["ubuntu-precise"])
    r = c.Create("parent_os")
    SetProps(r, parent_knobs)
    VerifyProps(r, parent_knobs)
    snap_parent_os = SnapshotProps(r)

    os_knobs = [
        ("virt_mode", "os"),
        ("porto_namespace", "parent"),
        ("bind", "{} /portobin ro; {} /portosrc ro".format(portobin, portosrc)),
        ("hostname", "shiny_os_container"),
        ("root_readonly", False),
        ("cpu_policy", "normal"),
        ("memory_limit", "1024000000"),
        ("command", "/sbin/init"),
        ("env", "VIRT_MODE=os;BIND=;HOSTNAME=shiny_new_container;"\
                "ROOT_READONLY=false;CPU_POLICY=normal;COMMAND=/sbin/init;"\
                "NET=macvlan eth0 eth0;"\
                "ROOT={};RECHARGE_ON_PGFAULT=true".format(v.path)),
        ("net", "macvlan eth0 eth0"),
        ("root", v.path),
        ("recharge_on_pgfault", True)
    ]

    r = c.Create("parent_os/os")
    SetProps(r, os_knobs)
    VerifyProps(r, os_knobs)
    snap_os = SnapshotProps(r)
    r.Start()

    rt_parent_knobs = [
        ("private", "rt_parent"),
        ("respawn", False),
        ("ulimit", "data: 16000000 32000000; memlock: 4096 4096; nofile: 100 200; nproc: 500 1000; "),
        ("isolate", True),
        ("env", "CONTAINER=porto;PARENT=1")
    ]

    r = c.Create("rt_parent")
    SetProps(r, rt_parent_knobs)
    VerifyProps(r, rt_parent_knobs)
    snap_rt_parent = SnapshotProps(r)

    rt_app_knobs = [
        ("cpu_limit", "4c"),
        ("private", "rt_app"),
        ("respawn", False),
        ("cpu_policy", "rt"),
        ("memory_guarantee", "2000000000"),
        ("command", "sleep 20"),
        ("memory_limit", "2100000000"),
        ("cwd", portosrc),
        ("net_limit", "default: 0"),
        ("cpu_guarantee", "4c"),
        ("ulimit", "data: 16000000 32000000; memlock: 4096 4096; nofile: 100 200; nproc: 500 1000; "),
        ("recharge_on_pgfault", True),
        ("isolate", False),
        ("env", "CONTAINER=porto;PARENT=1;TAG=mytag mytag2 mytag3")
    ]

    r = c.Create("rt_parent/rt_app")
    SetProps(r, rt_app_knobs)
    VerifyProps(r, rt_app_knobs)
    snap_rt_app = SnapshotProps(r)
    r.Start()
    legacy_rt_settings = DumpLegacyRt(r)

    c.disconnect()

    AsRoot()

    print " - upgrade"

    subprocess.check_call([portod, "upgrade"])

#That means we've upgraded successfully

    AsAlice()

    c = porto.Connection(timeout=3)

    ver, rev = c.Version()
    ExpectNe(ver, PREV_VERSION)
    CheckNetworkProblems()

    c.Wait(["test"])

#Checking if we can create subcontainers successfully (cgroup migration involved)

    r = c.Create("a")
    r.SetProperty("command", "bash -c '" + portoctl + " run -W self/a command=\"echo 123\"'")
    r.Start()
    assert r.Wait() == "a"
    assert r.GetProperty("exit_status") == "0"

    r2 = c.Find("a/a")
    r2.Wait() == "a/a"
    assert r2.GetProperty("exit_status") == "0"
    assert r2.GetProperty("stdout") == "123\n"
    r2.Destroy()
    r.Destroy()

    assert c.GetProperty("test", "exit_status") == "0"

    r = c.Find("parent_app")
    VerifyProps(r, parent_knobs)
    VerifySnapshot(r, snap_parent_app)
    CheckCaps(r)

    r = c.Find("parent_app/app")
    VerifyProps(r, app_knobs)
    VerifySnapshot(r, snap_app)
    CheckCaps(r)

    r = c.Find("parent_os")
    VerifyProps(r, parent_knobs)
    VerifySnapshot(r, snap_parent_os)
    CheckCaps(r)

    r = c.Find("parent_os/os")
    VerifyProps(r, os_knobs)
    VerifySnapshot(r, snap_os)
    CheckCaps(r)

    r = c.Find("rt_parent")
    VerifyProps(r, rt_parent_knobs)
    VerifySnapshot(r, snap_rt_parent)
    CheckCaps(r)

    r = c.Find("rt_parent/rt_app")
    VerifyProps(r, rt_app_knobs)
    VerifySnapshot(r, snap_rt_app)
    CheckCaps(r)

    CheckRt(r)

finally:
    AsRoot()
    ConfigurePortod('test-prev_release_upgrade', "")

c.disconnect()

print " - downgrade"

subprocess.check_call([prev_portod, "upgrade"])

AsAlice()

c = porto.Connection(timeout=3)

ver, rev = c.Version()
ExpectEq(ver, PREV_VERSION)
CheckNetworkProblems()

r = c.Find("test2")
assert r.Wait() == "test2"
assert r.GetProperty("stdout") == "456\n"
assert r.GetProperty("exit_status") == "0"

assert c.GetProperty("parent_os/os", "state") == "running"

r = c.Find("parent_app")
VerifyProps(r, parent_knobs)
VerifySnapshot(r, snap_parent_app)
CheckCaps(r)

r = c.Find("parent_app/app")
VerifyProps(r, app_knobs)
VerifySnapshot(r, snap_app)
CheckCaps(r)

r = c.Find("parent_os")
VerifyProps(r, parent_knobs)
CheckCaps(r)

r = c.Find("parent_os/os")
VerifyProps(r, os_knobs)
VerifySnapshot(r, snap_os)
CheckCaps(r)

r = c.Find("rt_parent")
VerifyProps(r, rt_parent_knobs)
VerifySnapshot(r, snap_rt_parent)
CheckCaps(r)

r = c.Find("rt_parent/rt_app")
VerifyProps(r, rt_app_knobs)
VerifySnapshot(r, snap_rt_app)
CheckCaps(r)
assert legacy_rt_settings == DumpLegacyRt(r)

AsRoot()

print " - restart to new vetsion"

RestartPortod()

ver, rev = c.Version()
ExpectNe(ver, PREV_VERSION)
CheckNetworkProblems()

shutil.rmtree(TMPDIR)
