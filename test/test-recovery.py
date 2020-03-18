#!/usr/bin/python -u

import os
import porto
import sys
import signal
import time
import subprocess
import shutil
import traceback
from test_common import *

def AsRoot():
    os.setresuid(0,0,0)

def ValidateDefaultProp(r):
    ExpectException(r.GetProperty, porto.exceptions.InvalidValue, "command[1]")
    ExpectException(r.SetProperty, porto.exceptions.InvalidValue, "command[1]", "ls")

    ref = {
            "command" : "",
            "env" : "",
            "cwd" : "/place/porto/" + r.name,
            "root" : "/",
            "bind" : "",
            "root_readonly" : False,
            "user" : "porto-alice",
            "group" : "porto-alice",
            "cpu_policy" : "normal",
            "cpu_limit" : "0c",
            "cpu_guarantee" : "0c",
            "memory_guarantee" : "0",
            "net" : "inherited",
            "respawn" : False,
            "stdin_path" : "/dev/null",
            "stdout_path" : "stdout",
            "stderr_path" : "stderr",
            "stdout_limit" : "8388608",
            "ulimit" : "",
            "hostname" : "",
            "devices" : "",
            "capabilities" : "CHOWN;DAC_OVERRIDE;FOWNER;FSETID;" +
            "KILL;SETGID;SETUID;SETPCAP;LINUX_IMMUTABLE;NET_BIND_SERVICE;" +
            "NET_ADMIN;NET_RAW;IPC_LOCK;SYS_CHROOT;SYS_PTRACE;SYS_ADMIN;SYS_BOOT;" +
            "SYS_NICE;SYS_RESOURCE;MKNOD;AUDIT_WRITE;SETFCAP",
            "isolate" : True,
            "enable_porto" : True,
            "private" : "",
            "max_respawns" : "",
          }

    for p in ref:
        value = r.GetProperty(p)
        if value != ref[p]:
            raise AssertionError("Default property {} has invalid value {} != {}".format(
                                 p, value, ref[p]))

    #Unsupported ones, we've already checked their existence in test_holder,
    #so let's just poke them
    ref = {
            "io_limit" : "",
            "io_ops_limit" : "",
            "memory_guarantee" : "0",
            "recharge_on_pgfault" : False,
          }

    for p in ref:
        try:
            value = r.GetProperty(p)
            if value != ref[p]:
                raise AssertionError("Default property {} has invalid value {} != {}".format(
                                     p, value, ref[p]))
        except porto.exceptions.NotSupported:
            pass

def ValidateDefaultData(r):
    ExpectException(r.GetData, porto.exceptions.InvalidProperty, "__invalid_data__")

    ref = { "state" : "stopped", "max_respawns" : "", "parent" : "/" }
    for d in ref:
        ExpectProp(r, d, ref[d])

    for d in ["exit_status", "root_pid", "stdout", "stderr", "cpu_usage",\
              "memory_usage", "minor_faults", "major_faults", "max_rss",\
               "oom_killed", "io_read", "io_write", "io_ops" ]:
        try:
            r.GetData(d)
            raise BaseException("Data {} accessible in the wrong state!".format(d))
        except (porto.exceptions.InvalidState, porto.exceptions.NotSupported):
            pass

    r.GetData("respawn_count")

def ValidateRunningData(r):
    ExpectException(r.GetData, porto.exceptions.InvalidProperty, "__invalid_data__")

    pid = r.GetData("root_pid")
    assert pid != "" and pid != "-1" and pid != "0",\
           "{} pid should be valid, not {}".format(r, pid)

    ref = { "state" : "running", "respawn_count" : "0", "parent" : "/" }
    for d in ref:
        if r.GetData(d) != ref[d]:
            raise AssertionError("Default data {} has invalid value {} != {}".format(
                                 p, value, ref[p]))

    ExpectException(r.GetData, porto.exceptions.InvalidState, "exit_status")
    ExpectException(r.GetData, porto.exceptions.InvalidState, "oom_killed")

    for d in ["io_read", "io_write", "io_ops", "stdout", "stderr", "cpu_usage",\
              "memory_usage"]:
        try:
            r.GetData(d)
        except porto.exceptions.NotSupported:
            pass

    ExpectPropGe(r, "minor_faults", 1)
    ExpectPropGe(r, "major_faults", 0)

    try:
        ExpectPropGe(r, "max_rss", 0)
    except porto.exceptions.NotSupported:
        pass

def RespawnTicks(r):
    old = r.GetData("respawn_count")
    tick = 0
    for i in range(0,5):
        time.sleep(1)
        new = r.GetData("respawn_count")
        if old != new:
            tick += 1
            old = new
    #Suppose that respawn will likely tick at least 2 times in 5s
    #otherwise there is some issues there
    ExpectLe(3, tick)


def TestRecovery():
    #Former selftest.cpp TestRecovery()
    print "Make sure we can restore stopped child when parent is dead"

    if os.getuid() == 0:
        AsAlice()

    c = porto.Connection(timeout=30)

    parent = c.Create("parent")
    parent.SetProperty("command", "sleep 1")
    child = c.Create("parent/child")
    child.SetProperty("command", "sleep 2")
    parent.Start()
    child.Start()
    child.Stop()
    parent.Wait(timeout=2000)

    AsRoot()
    KillPid(GetMasterPid(), signal.SIGKILL)
    subprocess.check_call([portod, "start"])
    AsAlice()
    c.connect()

    l = c.List()
    ExpectEq(len(l), 2)
    ExpectEq(l[0], "parent")
    ExpectEq(l[1], "parent/child")

    c.Destroy("parent")

    print "Make sure we can figure out that containers are dead even if master dies"

    r = c.Create("a:b")
    r.SetProperty("command", "sleep 3")
    r.Start()

    AsRoot()
    KillPid(GetMasterPid(), signal.SIGKILL)
    subprocess.check_output([portod, "start"])
    AsAlice()
    c.connect()

    ExpectEq(c.Wait(["a:b"]), "a:b")
    c.Destroy("a:b")

    print "Make sure we don't kill containers when doing recovery"

    c.disconnect()
    AsRoot()
    c.connect()

    props = {"command" : "sleep 1000",\
             "user" : "porto-alice",\
             "group" : "porto-bob",\
             "env" : "a=a;b=b"}

    r = c.Create("a:b")
    for p in props:
        r.SetProperty(p, props[p])
    r.Start()
    r.SetProperty("private", "ISS-AGENT")
    pid = int(r.GetData("root_pid"))

    Expect(IsRunning(pid))
    Expect(not IsZombie(pid))

    KillPid(GetPortodPid(), signal.SIGKILL)
    c.connect()

    ExpectProp(r, "state", "running")
    ExpectProp(r, "root_pid", str(pid))

    Expect(IsRunning(pid))
    Expect(not IsZombie(pid))

    for p in props:
        ExpectProp(r, p, props[p])

    c.Destroy("a:b")

    c.disconnect()
    AsAlice()
    c.connect()

    print "Make sure meta gets correct state upon recovery"

    parent = c.Create("a")
    child = c.Create("a/b")
    parent.SetProperty("isolate", "true")
    child.SetProperty("command", "sleep 1000")
    child.Start()

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()

    ExpectProp(parent, "state", "meta")
    parent.Destroy()


    print "restore virt_mode=host meta + virt_mode=job child container"

    parent = c.Run("test-host", virt_mode='host', weak=False)
    child = c.Run("test-host/job", virt_mode='job', command="sleep 1000", weak=False)

    AsRoot()
    ReloadPortod()
    AsAlice()

    c.Connect()

    ExpectProp(parent, "state", "meta")
    ExpectProp(child, "state", "running")
    parent.Destroy()


    print "Make sure hierarchical recovery works"
    #Still as alice

    parent = c.Create("a")
    child = c.Create("a/b")
    parent.SetProperty("isolate", "false")
    child.SetProperty("command", "sleep 1000")
    child.Start()

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()
    parent = c.Find("a")
    child = c.Find("a/b")

    conts = c.List()
    ExpectEq(len(conts), 2)
    ExpectEq(conts[0], "a")
    ExpectEq(conts[1], "a/b")
    ExpectProp(parent, "state", "meta")

    try:
        parent.SetProperty("recharge_on_pgfault", "true")
    except porto.exceptions.NotSupported:
        pass

    ExpectException(parent.SetProperty, porto.exceptions.InvalidState, "env", "a=b")
    ExpectProp(child, "state", "running")

    parent.Destroy()

    print "Make sure task is moved to correct cgroup on recovery"

    #Use a_b instead of a:b in selftest to simplify cgroup parsing
    r = c.Create("a_b")
    r.SetProperty("command", "sleep 1000")
    r.Start()
    pid = r.GetData("root_pid")

    AsRoot()
    open("/sys/fs/cgroup/memory/cgroup.procs","w").write(pid)

    for cg in open("/proc/" + pid + "/cgroup").readlines():
        if cg.find("memory") >= 0:
            ExpectEq(cg.split(":")[2].rstrip('\n'), "/")

    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()

    pid = c.GetData("a_b", "root_pid")

    cgs = {}
    for cg in open("/proc/" + pid + "/cgroup").readlines():
        (subsys, path) = cg.split(":")[1:3]
        for i in subsys.split(','):
            cgs[i] = path.rstrip('\n')

    ExpectEq(cgs["freezer"], "/porto/a_b")
    for i in ["memory","cpu","cpuacct","devices"]:
        ExpectEq(cgs[i], "/porto%a_b")

    c.Destroy("a_b")


    print "Make sure some data is persistent"

    r = c.Create("a:b")
    r.SetProperty("command", "sort -S 1G /dev/urandom")
    r.SetProperty("memory_limit", "32M")
    r.Start()
    r.Wait(timeout=60000)

    ExpectPropNe(r, "exit_status", "0")
    ExpectProp(r, "oom_killed", True)

    AsRoot()
    KillPid(GetPortodPid(), 9)
    AsAlice()
    c.connect()

    r = c.Find("a:b")

    ExpectPropNe(r, "exit_status", "0")
    ExpectProp(r, "oom_killed", True)

    r.Stop()
    r.SetProperty("command", "false")
    r.SetProperty("memory_limit", "0")
    r.SetProperty("respawn", "true")
    r.SetProperty("max_respawns", "1")
    r.Start()
    r.Wait(timeout=10000)

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()

    r = c.Find("a:b")
    r.GetData("respawn_count") == "1"

    print "Make sure stopped state is persistent"

    r.Destroy()
    r = c.Create("a:b")
    ValidateDefaultProp(r)
    ValidateDefaultData(r)

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()

    r = c.Find("a:b")
    ExpectProp(r, "state", "stopped")

    ValidateDefaultProp(r)
    ValidateDefaultData(r)


    print "Make sure paused state is persistent"

    r.SetProperty("command", "sleep 1000")
    r.Start()

    ValidateRunningData(r)
    state = GetState(r.GetData("root_pid"))
    Expect(state == "S" or state == "R")

    r.Pause()
    ExpectNe(GetState(r.GetData("root_pid")), "")

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()

    ExpectNe(GetState(r.GetData("root_pid")), "")

    r.Resume()

    ValidateRunningData(r)
    state = GetState(r.GetData("root_pid"))
    Expect(state == "S" or state == "R")

    time.sleep(1.0)

    ExpectPropNe(r, "time", "0")
    r.Destroy()

    print "Make sure respawn_count ticks after recovery"

    r = c.Create("a:b")
    r.SetProperty("command", "true")
    r.SetProperty("respawn", "true")
    r.Start()

    RespawnTicks(r)

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c.connect()

    RespawnTicks(r)
    r.Destroy()

    n = 100

    print "Make sure we can recover", n, "containers "

    for i in range(0, n):
        r = c.Create("recover" + str(i))
        r.SetProperty("command", "sleep 1000")
        r.Start()

    ExpectEq(len(c.List()), n)
    #ExpectException(c.Create, porto.exceptions.ResourceNotAvailable, "max_plus_one")

    c.disconnect()
    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    AsAlice()
    c = porto.Connection(timeout=300)

    ExpectEq(len(c.List()), n)

    for i in range(0, n):
        c.Kill("recover" + str(i), 9)

    for i in range(0, n):
        c.Destroy("recover" + str(i))

    c.disconnect()
    c = porto.Connection(timeout=30)

#Former selftest.cpp TestWaitRecovery()
def TestWaitRecovery():
    print "Check wait for restored container"

    if os.getuid() == 0:
        AsAlice()

    c = porto.Connection(timeout=30)

    aaa = c.Create("aaa")
    aaa.SetProperty("command", "sleep 3")
    aaa.Start()

    AsRoot()
    KillPid(GetPortodPid(), signal.SIGKILL)
    c.connect()

    aaa = c.Find("aaa")
    ExpectEq(aaa.Wait(timeout=3000), "aaa")
    ExpectProp(aaa, "state", "dead")

    aaa.Stop()

    print "Check wait for lost and restored container"

    aaa.SetProperty("command", "sleep 3")
    aaa.Start()

    AsRoot()
    KillPid(GetMasterPid(), signal.SIGKILL)
    subprocess.check_call([portod, "start"])
    c.connect()

    aaa = c.Find("aaa")
    ExpectEq(aaa.Wait(timeout=3000), "aaa")
    ExpectProp(aaa, "state", "dead")
    aaa.Destroy()

#Former selftest.cpp TestVolumeRecovery
def TestVolumeRecovery():
    print "Make sure porto removes leftover volumes"

    if os.getpid() != 0:
        AsRoot()

    c = porto.Connection(timeout=30)

    try:
        shutil.rmtree("/tmp/volume_c")
    except OSError:
        pass

    os.mkdir("/tmp/volume_c", 0755)

    ExpectEq(len(c.ListVolumes()), 0)

    limited = c.CreateVolume("/tmp/volume_c", space_limit="100m", inode_limit="1000")
    unlimited = c.CreateVolume()

    try:
        shutil.rmtree("/place/porto_volumes/leftover_volume")
    except OSError:
        pass

    os.mkdir("/place/porto_volumes/leftover_volume", 0755)

    KillPid(GetPortodPid(), signal.SIGKILL)
    c.connect()

    ExpectEq(len(c.ListVolumes()), 2)

    Expect(not os.path.exists("/place/porto_volumes/leftover_volume"))

    print "Make sure porto preserves mounted loop/overlayfs"

    ExpectEq(len(c.ListVolumes()), 2)

    mounts = [mount.split()[4] for mount in open("/proc/self/mountinfo", "r").readlines()]
    Expect(limited.path in mounts)
    Expect(unlimited.path in mounts)

    limited.Unlink()
    unlimited.Unlink()

    mounts = [mount.split()[4] for mount in open("/proc/self/mountinfo", "r").readlines()]
    Expect(not limited.path in mounts)
    Expect(not unlimited.path in mounts)

    os.rmdir("/tmp/volume_c")

def TestTCCleanup():
    print "Make sure stale tc classes to be cleaned up"

    AsRoot()

    c = porto.Connection(timeout=30)

    c.connect()

    subprocess.check_call([portod, "--discard", "restart"])

    kvs = set(os.listdir("/run/porto/kvs"))

    c.Create("a")
    r = c.Create("a/b")
    r.SetProperty("net_limit", "default: 1024")
    r.SetProperty("command", "sleep 10000")
    r.Start()

    kvs2 = set(os.listdir("/run/porto/kvs"))

    for f in kvs2 - kvs:
        os.unlink("/run/porto/kvs/" + f)

    subprocess.check_call([portod, "reload"])

    r = c.Create("a")
    r.SetProperty("command", "sleep 100")
    r.Start()

    r = c.Create("b")
    r.SetProperty("command", "sleep 100")
    r.Start()

    c.Destroy("a")
    c.Destroy("b")

    ExpectEq(c.GetProperty("/", "porto_stat[errors]"), "0")
    ExpectEq(c.GetProperty("/", "porto_stat[warnings]"), "0")

    c.disconnect()
    subprocess.check_call([portod, "--discard", "restart"])

    c.connect()

    c.Create("a")

    r = c.Create("a/b")
    r.SetProperty("net_limit", "default: 1023")
    r.SetProperty("command", "sleep 100")

    r = c.Create("a/b/c")

    r = c.Create("a/b/c/d")
    r.SetProperty("net_limit", "default: 512")
    r.SetProperty("command", "sleep 100")

    r.Start()

    subprocess.check_call([portod, "reload"])

    c.connect()

    ExpectEq(c.Find("a").name, "a")
    ExpectEq(c.Find("a/b").name, "a/b")
    ExpectEq(c.Find("a/b/c").name, "a/b/c")
    ExpectEq(c.Find("a/b/c/d").name, "a/b/c/d")

    ExpectEq(c.GetProperty("/", "porto_stat[errors]"), "0")
    ExpectEq(c.GetProperty("/", "porto_stat[warnings]"), "0")

    c.Destroy("a")

def TestPersistentStorage():
    print "Verifying volume persistent storage behavior"

    if os.getuid() == 0:
        AsAlice()

    c = porto.Connection(timeout=30)

    r = c.Create("test")
    base = c.CreateVolume(None, layers=["ubuntu-precise"], storage="test-persistent-base")
    ExpectEq(len(c.ListStorages()), 1)

    r.SetProperty("root", base.path)
    r.SetProperty("command", "bash -c \'echo 123 > 123.txt\'")
    r.Start()
    r.Wait()
    ExpectProp(r, "exit_status", "0")

    AsRoot()
    subprocess.check_call([portod, "restart"])
    AsAlice()

    ExpectEq(len(c.ListStorages()), 1)
    r = c.Create("test")
    base = c.CreateVolume(None, layers=["ubuntu-precise"], storage="test-persistent-base")

    r.SetProperty("root", base.path)
    r.SetProperty("command", "cat 123.txt")
    r.Start()
    r.Wait()
    ExpectProp(r, "exit_status", "0")
    ExpectProp(r, "stdout", "123\n")
    r.Stop()

    os.mkdir(base.path + "/loop")
    loop = c.CreateVolume(base.path + "/loop", backend="loop", storage="test-persistent-loop", space_limit="1G")
    ExpectEq(len(c.ListStorages()), 2)

    r.SetProperty("command", "bash -c \'echo 789 > /loop/loop.txt\'")
    r.Start()
    r.Wait()
    r.GetProperty("exit_status") == "0"

    AsRoot()
    subprocess.check_call([portod, "restart"])
    AsAlice()

    ExpectEq(len(c.ListStorages()), 2)
    r = c.Create("test")
    base = c.CreateVolume(None, layers=["ubuntu-precise"], storage="test-persistent-base")
    loop = c.CreateVolume(base.path + "/loop", backend="loop", storage="test-persistent-loop", space_limit="1G")
    r.SetProperty("root", base.path)
    r.SetProperty("command", "cat /loop/loop.txt")
    r.Start()
    r.Wait()
    ExpectProp(r, "exit_status", "0")
    ExpectProp(r, "stdout", "789\n")
    r.Stop()

    ExpectException(c.RemoveStorage, porto.exceptions.Busy, "test-persistent-loop")
    loop.Unlink()
    c.RemoveStorage("test-persistent-loop")
    ExpectEq(len(c.ListStorages()), 1)

    os.mkdir(base.path + "/native")
    native = c.CreateVolume(base.path + "/native", backend="native", storage="test-persistent-native")
    ExpectEq(len(c.ListStorages()), 2)

    r.SetProperty("command", "bash -c \'echo abcde > /native/abcde.txt\'")
    r.Start()
    r.Wait()
    ExpectProp(r, "exit_status", "0")

    AsRoot()
    subprocess.check_call([portod, "restart"])
    AsAlice()
    ExpectEq(len(c.ListStorages()), 2)

    r = c.Create("test")
    base = c.CreateVolume(None, layers=["ubuntu-precise"], storage="test-persistent-base")
    native = c.CreateVolume(base.path + "/native", backend="native", storage="test-persistent-native")
    ExpectEq(len(c.ListStorages()), 2)

    r.SetProperty("root", base.path)
    r.SetProperty("command", "cat /native/abcde.txt")
    r.Start()
    r.Wait()
    ExpectProp(r, "exit_status", "0")
    ExpectProp(r, "stdout", "abcde\n")
    r.Destroy()

    base.Unlink()
    c.RemoveStorage("test-persistent-base")
    c.RemoveStorage("test-persistent-native")

    os.mkdir("/tmp/test-recover-place")
    os.mkdir("/tmp/test-recover-place/porto_layers")
    os.mkdir("/tmp/test-recover-place/porto_volumes")
    os.mkdir("/tmp/test-recover-place/porto_storage")

    v = c.CreateVolume(None, place="/tmp/test-recover-place", storage="test", backend="native", private="some_private_value")
    ExpectEq(len(c.ListStorages(place="/tmp/test-recover-place")), 1)
    f = open(v.path + "/test.txt", "w")
    f.write("testtesttest")
    f.close()

    AsRoot()
    subprocess.check_call([portod, "restart"])
    AsAlice()

    v = c.CreateVolume(None, place="/tmp/test-recover-place", storage="test", backend="native")
    ExpectEq(len(c.ListStorages(place="/tmp/test-recover-place")), 1)
    ExpectEq(v.GetProperty('private'), "some_private_value")
    s = c.ListStorages(place="/tmp/test-recover-place")[0]
    ExpectEq(s.private_value, "some_private_value")
    f = open(v.path + "/test.txt", "r").read() == "testtesttest\n"

    v.Unlink()
    c.RemoveStorage("test", place="/tmp/test-recover-place")

    ExpectEq(len(c.ListStorages(place="/tmp/test-recover-place")), 0)
    ExpectEq(len(c.ListStorages()), 0)

def TestAttach():
    print "Verify that tids attached correctly after reload"

    def GetPidsAndTids(name):
        pids = {int(p.rstrip()) for p in open("/sys/fs/cgroup/freezer/porto/{}/cgroup.procs".format(name), 'r')}
        tids = {int(p.rstrip()) for p in open("/sys/fs/cgroup/freezer/porto/{}/tasks".format(name), 'r')}
        return pids, tids

    c = porto.Connection(timeout=30)

    a = c.Create("a")
    a.SetProperty("command", "python {}/multi-thread_app.py".format(os.path.dirname(os.path.realpath(__file__))))
    a.SetProperty("isolate", "false")

    a.Start()

    b = c.Create("a/b")
    b.SetProperty("command", "sleep 1000")
    b.SetProperty("isolate", "false")

    b.Start()
    time.sleep(1)

    ExpectProp(a, "state", "running")
    ExpectProp(b, "state", "running")

    #Get pids and tids before attach
    pids_a_1, tids_a_1 = GetPidsAndTids(a.name)
    pids_b_1, tids_b_1 = GetPidsAndTids(b.name)

    tids_a = list(tids_a_1 - pids_a_1)
    if len(tids_a) == 0:
        raise Exception("can't find tids for ct a")
    c.AttachThread(b.name, tids_a[0])

    #save cgroups for pids and tids to check after attach
    cgroups = {}
    for pid in tids_a_1.union(tids_a_1, pids_b_1, tids_b_1):
        cgroups[pid] = open("/proc/{}/cgroup".format(pid), 'r').read()

    #Get pids and tids after attach
    pids_a_2, tids_a_2 = GetPidsAndTids(a.name)
    pids_b_2, tids_b_2 = GetPidsAndTids(b.name)

    AsRoot()
    subprocess.check_call([portod, "reload"])
    AsAlice()

    time.sleep(1)

    #Get and check pids and tids after reload
    pids_a_3, tids_a_3 = GetPidsAndTids(a.name)
    pids_b_3, tids_b_3 = GetPidsAndTids(b.name)

    for tid, cgroup in cgroups.items():
        assert cgroup == open("/proc/{}/cgroup".format(tid), 'r').read()

    b.Destroy()
    a.Destroy()

    assert pids_a_2 == pids_a_3
    assert tids_a_2 == tids_a_3

    assert pids_b_2 == pids_b_3
    assert tids_b_2 == tids_b_3

subprocess.check_call([portod, "--verbose", "reload"])
ret = 0

try:
    TestRecovery()
    TestWaitRecovery()
    TestVolumeRecovery()
    TestTCCleanup()
    TestPersistentStorage()
    TestAttach()
except BaseException as e:
    print traceback.format_exc()
    ret = 1

AsRoot()
c = porto.Connection(timeout=30)

for r in c.ListContainers():
    try:
        r.Destroy()
    except:
        pass

for v in c.ListVolumes():
    try:
        v.Unlink()
    except:
        pass

for s in c.ListStorages():
    try:
        s.RemoveStorage()
    except:
        pass

if os.path.exists("/tmp/test-recover-place"):
    for s in c.ListStorages(place="/tmp/test-recover-place"):
        try:
            s.RemoveStorage()
        except:
            pass

if os.path.exists("/tmp/volume_c"):
    shutil.rmtree("/tmp/volume_c")

if os.path.exists("/place/porto_volumes/leftover_volume"):
    shutil.rmtree("/place/porto_volumes/leftover_volume")

if os.path.exists("/tmp/test-recover-place"):
    shutil.rmtree("/tmp/test-recover-place")

subprocess.check_call([portod, "--verbose", "--discard", "reload"])

sys.exit(ret)
