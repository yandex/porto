#!/usr/bin/python -u

import os
import porto
import sys
import signal
import time
import subprocess
import psutil
import shutil
from test_common import *

def SwitchRoot():
    os.setresuid(0,0,0)

def ValidateDefaultProp(r):
    assert Catch(r.GetProperty, "command[1]") == porto.exceptions.InvalidValue
    assert Catch(r.SetProperty, "command[1]", "ls") == porto.exceptions.InvalidValue

    ref = {\
            "command" : "", "cwd" : "/place/porto/" + r.name, "root" : "/",\
            "user" : "porto-alice", "group" : "porto-alice", "env" : "",\
            "memory_limit" : "0", "cpu_policy" : "normal",\
            "cpu_limit" : str(psutil.cpu_count()) + "c",\
            "cpu_guarantee" : "0c", "net_priority[default]" : "3", "net" : "inherited",\
            "respawn" : False, "memory_limit" : "0", "stdin_path" : "/dev/null",\
            "stdout_path" : "stdout", "stderr_path" : "stderr", "ulimit" : "",\
            "hostname" : "", "bind_dns" : True, "devices" : "",\
            "capabilities" : "CHOWN;DAC_OVERRIDE;FOWNER;FSETID;" +\
            "KILL;SETGID;SETUID;SETPCAP;LINUX_IMMUTABLE;NET_BIND_SERVICE;" +\
            "NET_ADMIN;NET_RAW;IPC_LOCK;SYS_CHROOT;SYS_PTRACE;SYS_ADMIN;SYS_BOOT;" +\
            "SYS_NICE;SYS_RESOURCE;MKNOD;AUDIT_WRITE;SETFCAP",\
            "isolate" : True, "stdout_limit" : "8388608", "private" : "",\
            "bind" : "", "root_readonly" : False, "max_respawns" : "-1",\
            "enable_porto" : True
          }

    for p in ref:
        value = r.GetProperty(p)
        if value != ref[p]:
            raise AssertionError("Default property {} has invalid value {} != {}".format(
                                 p, value, ref[p]))

    #Unsupported ones, we've already checked their existence in test_holder,
    #so let's just poke them
    ref = { "memory_guarantee" : "0", "io_policy" : "normal", "io_limit" : "0",\
            "io_ops_limit" : "0", "memory_guarantee" : "0",\
            "recharge_on_pgfault" : False,  }

    for p in ref:
        try:
            value = r.GetProperty(p)
            if value != ref[p]:
                raise AssertionError("Default property {} has invalid value {} != {}".format(
                                     p, value, ref[p]))
        except porto.exceptions.NotSupported:
            pass

def ValidateDefaultData(r):
    assert Catch(r.GetData, "__invalid_data__") == porto.exceptions.InvalidProperty

    ref = { "state" : "stopped", "max_respawns" : "-1", "parent" : "/" }
    for d in ref:
        assert r.GetData(d) == ref[d]

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
    assert Catch(r.GetData, "__invalid_data__") == porto.exceptions.InvalidProperty

    pid = r.GetData("root_pid")
    assert pid != "" and pid != "-1" and pid != "0"

    ref = { "state" : "running", "respawn_count" : "0", "parent" : "/" }
    for d in ref:
        if r.GetData(d) != ref[d]:
            raise AssertionError("Default data {} has invalid value {} != {}".format(
                                 p, value, ref[p]))

    assert Catch(r.GetData, "exit_status") == porto.exceptions.InvalidState
    assert Catch(r.GetData, "oom_killed") == porto.exceptions.InvalidState

    for d in ["io_read", "io_write", "io_ops", "stdout", "stderr", "cpu_usage",\
              "memory_usage"]:
        try:
            r.GetData(d)
        except porto.exceptions.NotSupported:
            pass

    assert int(r.GetData("minor_faults")) > 0
    assert int(r.GetData("major_faults")) >= 0

    try:
        assert int(r.GetData("max_rss")) >= 0
    except porto.exceptions.NotSupported:
        pass

def RespawnTicks(r):
    old = r.GetData("respawn_count")
    for i in range(0,3):
        time.sleep(1.1)
        new = r.GetData("respawn_count")
        assert old != new
        old = new


def TestRecovery(c):
    #Former selftest.cpp TestRecovery()
    print "Make sure we can restore stopped child when parent is dead"

    parent = c.Create("parent")
    parent.SetProperty("command", "sleep 1")
    child = c.Create("parent/child")
    child.SetProperty("command", "sleep 2")
    parent.Start()
    child.Start()
    child.Stop()
    parent.Wait(timeout=2000)

    SwitchRoot()
    KillPid(GetMasterPid(), signal.SIGKILL)
    subprocess.check_call([portod, "start"])
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    l = c.List()
    assert len(l) == 2
    assert l[0] == "parent"
    assert l[1] == "parent/child"

    c.Destroy("parent")

    print "Make sure we can figure out that containers are dead even if master dies"

    r = c.Create("a:b")
    r.SetProperty("command", "sleep 3")
    r.Start()

    SwitchRoot()
    KillPid(GetMasterPid(), signal.SIGKILL)
    subprocess.check_output([portod, "start"])
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    assert c.Wait(["a:b"]) == "a:b"
    c.Destroy("a:b")

    print "Make sure we don't kill containers when doing recovery"

    c.disconnect()
    SwitchRoot()
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
    assert IsRunning(pid)
    assert not IsZombie(pid)

    KillPid(GetSlavePid(), signal.SIGKILL)
    c.connect()

    c.GetData("a:b", "state") == "running"
    c.GetData("a:b", "root_pid") == str(pid)

    assert IsRunning(pid)
    assert not IsZombie(pid)

    for p in props:
        assert c.GetProperty("a:b", p) == props[p]

    c.Destroy("a:b")

    c.disconnect()
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    print "Make sure meta gets correct state upon recovery"

    parent = c.Create("a")
    child = c.Create("a/b")
    parent.SetProperty("isolate", "true")
    child.SetProperty("command", "sleep 1000")
    child.Start()

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    assert c.GetData("a", "state") == "meta"
    c.Destroy("a")

    print "Make sure hierarchical recovery works"
    #Still as alice

    parent = c.Create("a")
    child = c.Create("a/b")
    parent.SetProperty("isolate", "false")
    child.SetProperty("command", "sleep 1000")
    child.Start()

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()
    parent = c.Find("a")
    child = c.Find("a/b")

    conts = c.List()
    assert len(conts) == 2
    assert conts[0] == "a"
    assert conts[1] == "a/b"
    assert parent.GetData("state") == "meta"

    try:
        parent.SetProperty("recharge_on_pgfault", "true")
    except porto.exceptions.NotSupported:
        pass

    assert Catch(parent.SetProperty, "env", "a=b") == porto.exceptions.InvalidState
    assert child.GetData("state") == "running"

    parent.Destroy()

    print "Make sure task is moved to correct cgroup on recovery"

    #Use a_b instead of a:b in selftest to simplify cgroup parsing
    r = c.Create("a_b")
    r.SetProperty("command", "sleep 1000")
    r.Start()
    pid = r.GetData("root_pid")

    SwitchRoot()
    open("/sys/fs/cgroup/memory/cgroup.procs","w").write(pid)

    for cg in open("/proc/" + pid + "/cgroup").readlines():
        if cg.find("memory") >= 0:
            assert cg.split(":")[2].rstrip('\n') == "/"

    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    pid = c.GetData("a_b", "root_pid")

    cgs = {}
    for cg in open("/proc/" + pid + "/cgroup").readlines():
        (subsys, path) = cg.split(":")[1:3]
        for i in subsys.split(','):
            cgs[i] = path.rstrip('\n')

    assert cgs["freezer"] == "/porto/a_b"
    for i in ["memory","cpu","cpuacct","devices"]:
        assert cgs[i] == "/porto%a_b"

    c.Destroy("a_b")


    print "Make sure some data is persistent"

    r = c.Create("a:b")
    r.SetProperty("command", "sort -S 1G /dev/urandom")
    r.SetProperty("memory_limit", "32M")
    r.Start()
    r.Wait(timeout=60000)

    assert r.GetData("exit_status") == "9"
    assert r.GetData("oom_killed")

    SwitchRoot()
    KillPid(GetSlavePid(), 9)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    r = c.Find("a:b")

    assert r.GetData("exit_status") == "9"
    assert r.GetData("oom_killed")

    r.Stop()
    r.SetProperty("command", "false")
    r.SetProperty("memory_limit", "0")
    r.SetProperty("respawn", "true")
    r.SetProperty("max_respawns", "1")
    r.Start()
    r.Wait(timeout=10000)

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    r = c.Find("a:b")
    r.GetData("respawn_count") == "1"

    print "Make sure stopped state is persistent"

    r.Destroy()
    r = c.Create("a:b")
    ValidateDefaultProp(r)
    ValidateDefaultData(r)

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    r = c.Find("a:b")
    assert r.GetData("state") == "stopped"

    ValidateDefaultProp(r)
    ValidateDefaultData(r)


    print "Make sure paused state is persistent"

    r.SetProperty("command", "sleep 1000")
    r.Start()

    ValidateRunningData(r)
    state = GetState(r.GetData("root_pid"))
    assert state == "S" or state == "R"

    r.Pause()
    assert GetState(r.GetData("root_pid")) != ""

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    assert GetState(r.GetData("root_pid")) != ""

    r.Resume()

    ValidateRunningData(r)
    state = GetState(r.GetData("root_pid"))
    assert state == "S" or state == "R"

    time.sleep(1.0)

    assert r.GetData("time") != "0"
    r.Destroy()

    print "Make sure respawn_count ticks after recovery"

    r = c.Create("a:b")
    r.SetProperty("command", "true")
    r.SetProperty("respawn", "true")
    r.Start()

    RespawnTicks(r)

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c.connect()

    RespawnTicks(r)
    r.Destroy()

    print "Make sure we can recover huge number of containers "

    for i in range(0, 3000):
        r = c.Create("recover" + str(i))
        r.SetProperty("command", "sleep 1000")
        r.Start()

    assert len(c.List()) == 3000
    assert Catch(c.Create, "max_plus_one") == porto.exceptions.ResourceNotAvailable

    c.disconnect()
    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    SwitchUser("porto-alice", *GetUidGidByUsername("porto-alice"))
    c = porto.Connection(timeout=300)

    assert len(c.List()) == 3000

    for i in range(0, 3000):
        c.Kill("recover" + str(i), 9)

    for i in range(0, 3000):
        c.Destroy("recover" + str(i))

    c.disconnect()
    c = porto.Connection(timeout=30)

#Former selftest.cpp TestWaitRecovery()
def TestWaitRecovery(c):
    print "Check wait for restored container"

    aaa = c.Create("aaa")
    aaa.SetProperty("command", "sleep 3")
    aaa.Start()

    SwitchRoot()
    KillPid(GetSlavePid(), signal.SIGKILL)
    c.connect()

    aaa = c.Find("aaa")
    assert aaa.Wait(timeout=3000) == "aaa"
    assert aaa.GetData("state") == "dead"

    aaa.Stop()

    print "Check wait for lost and restored container"

    aaa.SetProperty("command", "sleep 3")
    aaa.Start()

    SwitchRoot()
    KillPid(GetMasterPid(), signal.SIGKILL)
    subprocess.check_call([portod, "start"])
    c.connect()

    aaa = c.Find("aaa")
    assert aaa.Wait(timeout=3000) == "aaa"
    assert aaa.GetData("state") == "dead"
    aaa.Destroy()

#Former selftest.cpp TestVolumeRecovery
def TestVolumeRecovery(c):
    print "Make sure porto removes leftover volumes"

    SwitchRoot()
    try:
        shutil.rmtree("/tmp/volume_c")
    except OSError:
        pass

    os.mkdir("/tmp/volume_c", 0755)

    assert len(c.ListVolumes()) == 0

    limited = c.CreateVolume("/tmp/volume_c", space_limit="100m", inode_limit="1000")
    unlimited = c.CreateVolume()

    try:
        shutil.rmtree("/place/porto_volumes/leftover_volume")
    except OSError:
        pass

    os.mkdir("/place/porto_volumes/leftover_volume", 0755)

    KillPid(GetSlavePid(), signal.SIGKILL)
    c.connect()

    assert not os.path.exists("/place/porto_volumes/leftover_volume")

    print "Make sure porto preserves mounted loop/overlayfs"

    assert len(c.ListVolumes()) == 2

    mounts = [mount.split()[4] for mount in open("/proc/self/mountinfo", "r").readlines()]
    assert limited.path in mounts
    assert unlimited.path in mounts

    limited.Unlink()
    unlimited.Unlink()

    mounts = [mount.split()[4] for mount in open("/proc/self/mountinfo", "r").readlines()]
    assert not limited.path in mounts
    assert not unlimited.path in mounts

    os.rmdir("/tmp/volume_c")

DropPrivileges()
c = porto.Connection(timeout=30)
TestRecovery(c)
TestWaitRecovery(c)
TestVolumeRecovery(c)
