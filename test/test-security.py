#!/usr/bin/python

import os
import time
import errno
import pwd
import grp
import sys
import os
import subprocess
import shutil
import tarfile

import porto
import test_common
from test_common import *


def PrepareLayer(c, name):
    try:
        c.ImportLayer(name, test_common.layers[name])
    except porto.exceptions.LayerAlreadyExists:
        pass
    except:
        raise

#stdin/stderr/stdout privilege escalation

def run_streams(r):
    r.SetProperty("command", "echo -n 654321")
    r.SetProperty("stdout_path", "/tmp/porto-tests/root-secret")
    assert Catch(r.Start) == porto.exceptions.InvalidValue

    r.SetProperty("command", "tee /proc/self/fd/2")
    r.SetProperty("stdin_path", "/tmp/porto-tests/porto-alice-stdin")
    r.SetProperty("stderr_path", "/tmp/porto-tests/root-secret")
    assert Catch(r.Start) == porto.exceptions.InvalidValue

    r.SetProperty("command", "cat")
    r.SetProperty("stdin_path", "/tmp/porto-tests/root-secret")
    r.SetProperty("stdout_path", "")
    r.SetProperty("stderr_path", "")
    assert Catch(r.Start) == porto.exceptions.InvalidValue


def std_streams_escalation():
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")
    (bob_uid, bob_gid) = GetUidGidByUsername("porto-bob")

    Catch(os.remove,"/tmp/porto-tests/root-secret")
    Catch(os.remove,"/tmp/porto-tests/porto-alice-stdin")

    f = open("/tmp/porto-tests/root-secret", "w")
    f.write("0123456789")
    os.fchmod(f.fileno(), 0600)
    f.close()

    SwitchUser("porto-alice", alice_uid, alice_gid)

    f = open("/tmp/porto-tests/porto-alice-stdin", "w+")
    f.write("123456")
    f.close()

    c = porto.Connection()
    r = c.Create("test")

    #run under user
    run_streams(r)

    SwitchRoot()

    c = porto.Connection()
    r = c.Find("test")

    #run under root
    run_streams(r)

    SwitchRoot()
    c.Destroy("test")
    os.remove("/tmp/porto-tests/root-secret")
    os.remove("/tmp/porto-tests/porto-alice-stdin")

#child escapes parent namespace (leaving chroot)

def ns_escape_container():
    w = int(sys.argv[2])
    to_kill = int(sys.argv[3])
    try:
        f = open("/tmp/porto-tests/root-secret", "r")
        print f.read()
        f.close()
        print "FAIL",
    except IOError as e:
        if e[0] == errno.ENOENT:
            print "OK",
            sys.stdout.flush()
            if w > 0:
                time.sleep(w)

            if to_kill:
                pid = os.getpid()
                os.kill(pid, 9)
        else:
            print "FAIL",
    except:
        print "FAIL",

    sys.stdout.flush()
    exit(0)

def ns_escape(v):
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")

    try:
        os.remove("/tmp/porto-tests/root-secret")
    except:
        pass

    f = open("/tmp/porto-tests/root-secret","w")
    f.write("123456")
    f.close()
    os.chmod("/tmp/porto-tests/root-secret", 0600)

    SwitchUser("porto-alice", alice_uid, alice_gid)

    c = porto.Connection()

    r = c.Create("parent")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    r.SetProperty("bind", os.getcwd() + " /porto ro")
    r.SetProperty("command", "python /porto/test/test-security.py ns_escape_container 2 1")
    r.SetProperty("porto_namespace", "parent")

    r = c.Create("parent/child")
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    #FIXME:
    #porto r.SetProperty("command","cat /porto/test/test-security.py") shows file contents, but
    #c.SetProperty("parent/child","command", "python /porto/test/test-security.py ns_escape_container 10 0") fails (file not found)
    r.SetProperty("command", "sleep 10")
    r.SetProperty("respawn", "true")
    r.SetProperty("max_respawns", "1")
    r.SetProperty("root_readonly","true")
    r.SetProperty("porto_namespace", "parent/child")
    r.Start()
    time.sleep(5)

    assert c.GetProperty("parent","state") == "dead" and c.GetProperty("parent/child", "state") == "dead"
    output = c.Get(["parent","parent/child"], ["stdout"])
    assert output["parent"]["stdout"] == "OK"

    #And now vice versa...

    c.Stop("parent")

    c.SetProperty("parent", "command", "python /porto/test/test-security.py ns_escape_container 3 0")
    #Actually, on fail because of ns escape
    #we won't even find our python test, but anyway...
    r.SetProperty("command", "python /porto/test/test-security.py ns_escape_container 0 1")

    r.Start()

    c.Wait(["parent"])
    output = c.Get(["parent","parent/child"], ["stdout"])
    assert output["parent"]["stdout"] == "OK\n"
    assert output["parent/child"]["stdout"] == "OK"

    SwitchRoot()

    c.Destroy("parent")
    os.unlink("/tmp/porto-tests/root-secret")

#binds privilege escalation

def read_shadow():
    f = open("/tmp/shadow", "r")
    print f.read()
    f.close()
    exit(0)

def append_sudoers():
    f = open("/tmp/sudoers", "a")
    print "Opened sudoers for append..."
    sys.stdout.flush()
    #f.write("\tmax7255 (ALL) NOPASSWD: ALL")
    f.close()
    exit(0)

def append_passwd():
    f = open("/tmp/passwd", "a")
    #f.write("joker:x:1980:1980:::/bin/false")
    print "Opened passwd for append..."
    sys.stdout.flush()
    f.close()
    exit(0)


def bind_escalation_container():
    exec(sys.argv[2]+"()")
    exit(0)

def binds_escalation(v):
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")
    (bob_uid, bob_gid) = GetUidGidByUsername("porto-bob")

    c = porto.Connection()

    SwitchUser("porto-alice", alice_uid, alice_gid)
    c = porto.Connection()
    r = c.Create("bind_file")
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    r.SetProperty("bind", os.getcwd() + " /porto ro")
    r.SetProperty("root", v.path)
    r.SetProperty("bind", "/etc/shadow /tmp/shadow ro")
    r.SetProperty("command", "python /porto/test/test-security.py bind_escalation_container read_shadow")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/etc/passwd /tmp/passwd rw")
    r.SetProperty("command", "python /porto/test/test-security.py bind_escalation_container append_passwd")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/etc/sudoers /tmp/sudoers rw")
    r.SetProperty("command", "python /porto/test/test-security.py bind_escalation_container append_sudoers")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/sbin /tmp/lol rw")
    r.SetProperty("command", "/tmp/lol/hwclock")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.Destroy()
    SwitchRoot()

    os.mkdir("/tmp/porto-tests/dir1")
    os.chmod("/tmp/porto-tests/dir1", 0777)
    os.mkdir("/tmp/porto-tests/mount1")
    os.chmod("/tmp/porto-tests/mount1", 0555)
    os.mkdir("/tmp/porto-tests/dir-bob")
    os.chmod("/tmp/porto-tests/dir-bob", 0700)
    os.chown("/tmp/porto-tests/dir-bob", bob_uid, bob_gid)

    SwitchUser("porto-alice", alice_uid, alice_gid)

    f = open("/tmp/porto-tests/dir1/file", "w+")
    f.write("123456")
    f.close()

    c = porto.Connection()
    r = c.Create("test")
    r.SetProperty("bind", "/tmp/porto-tests/dir1 /tmp/porto-tests/mount1/mount2 rw")
    r.SetProperty("command", "dd if=/dev/zero of=/tmp/porto-tests/mount1/mount2/file bs=32 count=1") 

    #We have warning about rights there, should be replaced with assert in the future
    warn_ctr_old = int(c.Get(["/"],["porto_stat[warnings]"])["/"]["porto_stat[warnings]"])
    r.Start()
    r.Wait()
    warn_ctr = int(c.Get(["/"],["porto_stat[warnings]"])["/"]["porto_stat[warnings]"])

    assert warn_ctr == warn_ctr_old + 1

    r.Stop()
    r.SetProperty("bind", "/tmp/porto-tests/dir-bob /tmp/porto-tests/mount1/mount2 rw")

    #We have warning about rights there, should be replaced with assert in the future
    warn_ctr_old = int(c.Get(["/"],["porto_stat[warnings]"])["/"]["porto_stat[warnings]"])
    r.Start()
    r.Wait()
    warn_ctr = int(c.Get(["/"],["porto_stat[warnings]"])["/"]["porto_stat[warnings]"])
    assert warn_ctr == warn_ctr_old + 1

    c.Destroy("test")
    SwitchRoot()

#privilege escalation for requests from inside the porto container w virt_mode=="os"

def internal_escalation_container():
    c = porto.Connection()
    r = c.Create("test_cont2")
    exit(0)

def internal_escalation(v):
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")

    c = porto.Connection(timeout=120)

    SwitchUser("porto-alice", alice_uid, alice_gid)
    c = porto.Connection()
    r = c.Create("test_cont1")
    r.SetProperty("virt_mode", "os")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    r.SetProperty("bind", os.getcwd() + " /porto ro")
    r.SetProperty("command", "python /porto/test/test-security.py internal_escalation_container")

    r.Start()
    r.Wait()

    assert c.GetProperty("test_cont2", "user") == "porto-alice"

    r.Destroy()
    c.Destroy("test_cont2")
    SwitchRoot()

#porto_namespace escape

def porto_namespace_escape_container():
    c = porto.Connection()
    c.SetProperty("self", "porto_namespace", "")
    exit(0)


def porto_namespace_escape(v):
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")

    SwitchUser("porto-alice", alice_uid, alice_gid)

    c = porto.Connection()
    r = c.Create("test")
    r.SetProperty("porto_namespace", "test")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    r.SetProperty("bind", os.getcwd() + " /porto ro")
    r.SetProperty("command", \
                  "python /porto/test/test-security.py porto_namespace_escape_container")
    r.Start()
    r.Wait()

    assert r.GetProperty("porto_namespace") == "test"
    assert r.GetData("exit_status") != "0"

    r.Destroy()

    SwitchRoot()

#layers privilege escalation/escape

def layer_escalation_container():
    #We can use e.g. /etc down there...
    os.symlink("/tmp/porto-tests", "porto-tests")

    t = tarfile.open(name="/layer0.tar", mode="w")
    t.add("porto-tests")
    t.close()

    os.remove("porto-tests")
    os.mkdir("porto-tests")

    #And we can place /etc/sudoers here...
    f = open("porto-tests/evil_file", "w")
    f.write("pwned")
    f.close()

    t = tarfile.open(name="/layer1.tar", mode="w")
    t.add("porto-tests/evil_file")
    t.close()

    c = porto.Connection()

    #We have persist layers here in porto, let's create clean layer for test
    try:
        c.RemoveLayer("test-layer")
    except:
        pass

    l = c.ImportLayer("test-layer", "/layer0.tar")
    l.Merge("/layer1.tar")

    c.RemoveLayer("test-layer")

    os.remove("porto-tests/evil_file")
    os.rmdir("porto-tests")
    os.remove("layer0.tar")
    os.remove("layer1.tar")

    exit(0)

def layer_escalation_volume_container():
    os.mkdir("layer")
    f = open("layer/good_file", "w")
    f.write("pwned")

    vol_path = sys.argv[2]
    c = porto.Connection()
    subprocess.check_call(["/porto/portoctl", "vcreate", "/layer", "path="+vol_path+\
                          "/../../../../tmp/porto-tests", "layers=/layer"])

    exit(0)

def layer_escalation(v):
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")

    c = porto.Connection(timeout=120)

    SwitchUser("porto-alice", alice_uid, alice_gid)
    c = porto.Connection()
    r = c.Create("test")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    r.SetProperty("bind", os.getcwd() + " /porto ro")
    r.SetProperty("command", "python /porto/test/test-security.py layer_escalation_container")

    r.Start()
    r.Wait()

    assert Catch(open, "/tmp/porto-tests/evil_file", "r") == IOError

    assert Catch(c.RemoveLayer, "../../../..") == porto.exceptions.InvalidValue
    assert Catch(c.ImportLayer, "../etc", "/tmp") == porto.exceptions.InvalidValue

    r.Destroy()

    SwitchRoot()

    f = open("/tmp/porto-tests/good_file", "w")
    f.write("I am a duck")
    f.close()

    SwitchUser("porto-alice", alice_uid, alice_gid)
    c = porto.Connection()
    r = c.Create("test")


    r.SetProperty("root", v.path)
    r.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
    r.SetProperty("command", "python /porto/test/test-security.py layer_escalation_volume_container " + v.path)
    r.SetProperty("stdout_path","/tmp/stdout")
    r.SetProperty("stderr_path","/tmp/stderr")
    r.SetProperty("bind", os.getcwd() + " /porto ro")

    r.Start()
    r.Wait()

    assert open("/tmp/porto-tests/good_file", "r").read() == "I am a duck"

    r.Destroy()

    SwitchRoot()

if len(sys.argv) > 1:
    exec(sys.argv[1]+"()")

if os.getuid() != 0:
    SwitchRoot()

c = porto.Connection(timeout=120)

PrepareLayer(c, "precise-base-porto")
v = c.CreateVolume(path=None, layers=["precise-base-porto"])

try:
    shutil.rmtree("/tmp/porto-tests")
except:
    pass

os.mkdir("/tmp/porto-tests")
os.chmod("/tmp/porto-tests", 0777)

std_streams_escalation()
binds_escalation(v)
internal_escalation(v)
porto_namespace_escape(v)
layer_escalation(v)
ns_escape(v)

shutil.rmtree("/tmp/porto-tests")
v.Unlink()
