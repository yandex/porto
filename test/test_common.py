from __future__ import print_function

import sys
import os
import signal
import pwd
import grp
import time
import platform
import re
import subprocess

portosrc = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
portobin = os.getcwd()
portoctl = portobin + "/portoctl"
portod = os.path.abspath(portobin + "/portod")
portotest = portobin + "/portotest"

try:
    import google.protobuf
except ImportError as e:
    print(e)
    sys.exit(0)

def Catch(func, *args, **kwargs):
    try:
        func(*args, **kwargs)
    except:
        return sys.exc_info()[0]
    return None

def Expect(a):
    assert a, "condition does not hold"

def ExpectEq(a, b):
    assert a == b, "{} should be equal {}".format(a, b)

def ExpectNe(a, b):
    assert a != b, "{} should not be equal {}".format(a, b)

def ExpectLe(a, b, descr=""):
    assert a <= b, "{}{} should be less or equal {}".format(descr, a, b)

def ExpectRange(a, l, h):
    assert l <= a <= h, "{} should be within {} .. {}".format(a, l, h)

def ExpectProp(ct, prop, val):
    cur = ct.GetProperty(prop)
    assert cur == val, "{} property {} should be {} not {}".format(ct, prop, val, cur)

def ExpectPropNe(ct, prop, val):
    cur = ct.GetProperty(prop)
    assert cur != val, "{} property {} value {} should be not equal to {}".format(ct, prop, val, cur)

def ExpectPropGe(ct, prop, val):
    cur = int(ct.GetProperty(prop))
    assert cur >= val, "{} property {} should be at least {} not {}".format(ct, prop, val, cur)

def ExpectPropLe(ct, prop, val):
    cur = int(ct.GetProperty(prop))
    assert cur <= val, "{} property {} should be at most {} not {}".format(ct, prop, val, cur)

def ExpectPropRange(ct, prop, low, high):
    cur = int(ct.GetProperty(prop))
    assert low <= cur <= high, "{} property {} should be within {} .. {} not {}".format(ct, prop, low, high, cur)

def ExpectException(func, exc, *args, **kwargs):
    tmp = Catch(func, *args, **kwargs)
    assert tmp == exc, "method {} should throw {} not {}".format(func, exc, tmp)

def ExpectFile(path, mode, dev=0):
    try:
        st = os.lstat(path)
        assert st.st_mode == mode, "file {} mode {} should be {}".format(path, st.st_mode, mode)
        assert st.st_rdev == dev, "device {} {:x} should be {:x}".format(path, st.st_rdev, dev)
    except OSError:
        assert mode == None, "file {} not found".format(path)

def ParseMountinfo(pid="self"):
    ret = {}
    for line in open("/proc/{}/mountinfo".format(pid), "r"):
        l = line.split()
        sep = l.index('-')
        m = { 'mount_id': l[0], 'parent_id': l[1], 'dev': l[2], 'bind': l[3], 'target': l[4], 'flag': l[5].split(','), 'type': l[sep+1], 'source': l[sep+2], 'opt': l[sep+3].split(',') }
        for tag in l[6:sep]:
            if ':' in tag:
                n, v = tag.split(':', 2)
                m[n] = v
            else:
                m[tag] = True
        # override lower mounts
        ret[m['target']] = m
    return ret

def ParseCgroup(pid):
    ret = {}
    for line in open("/proc/{}/cgroup".format(pid), "r"):
        l = line.split(':')
        ret[l[1]] = l[2].strip()
    return ret

def WithSystemd():
    return os.path.islink("/sbin/init") and os.readlink("/sbin/init").endswith("/systemd")

def GetSystemdCg(pid):
    return ParseCgroup(pid).get('name=systemd')

def MemoryStat(ct, stat):
    for line in ct.GetProperty("memory.stat").splitlines():
        k, v = line.split()
        if k == stat:
            return int(v)
    return None

def ExpectMemoryStatLe(ct, stat, val):
    cur = MemoryStat(ct, stat)
    assert cur is not None, "{} memory.stat:{} not found".format(ct, stat)
    assert cur <= val, "{} memory.stat:{} should be at most {} not {}".format(ct, stat, val, cur)

def UserId(name):
    try:
        return pwd.getpwnam(name).pw_uid
    except KeyError:
        return None

def GroupId(name):
    try:
        return grp.getgrnam(name).gr_gid
    except KeyError:
        return None

alice_uid=UserId("porto-alice")
alice_gid=GroupId("porto-alice")

bob_uid=UserId("porto-bob")
bob_gid=GroupId("porto-bob")

charlie_uid=UserId("porto-charlie")
charlie_gid=GroupId("porto-charlie")

david_uid=UserId("porto-david")
david_gid=GroupId("porto-david")

porto_gid=GroupId("porto")

def AsRoot():
    os.setresgid(0, 0, 0)
    os.setresuid(0, 0, 0)
    os.setgroups([0])

def SwitchUser(username, uid, gid):
    os.initgroups(username, uid)
    os.setresgid(gid, gid, 0)
    os.setresuid(uid, uid, 0)

def AsAlice():
    SwitchUser("porto-alice", alice_uid, alice_gid)

def AsBob():
    SwitchUser("porto-bob", bob_uid, bob_gid)

def AsCharlie():
    SwitchUser("porto-charlie", charlie_uid, charlie_gid)

def AsDavid():
    SwitchUser("porto-david", david_uid, david_gid)

def GetPortodPid():
    pid = int(open("/run/portod.pid").read())
    open("/proc/" + str(pid) + "/status").readline().index("portod")
    return pid

def GetMasterPid():
    pid = int(open("/run/portoloop.pid").read())
    open("/proc/" + str(pid) + "/status").readline().index("portod-master")
    return pid

def ReloadPortod():
    pid = GetPortodPid()
    os.kill(GetMasterPid(), signal.SIGHUP)
    try:
        for i in range(3000):
            os.kill(pid, 0)
            time.sleep(0.1)
        raise Exception("cannot reload porto")
    except OSError:
        pass

def StopPortod():
    global portod
    subprocess.check_call([portod, 'stop'])

def RestartPortod():
    # Note that is restart, not reload!
    # Thus we intentionally want to drop state

    global portod
    subprocess.check_call([portod, 'restart'])

def CleanupConfigs():
    for f in os.listdir('/etc/portod.conf.d'):
        os.unlink('/etc/portod.conf.d/%s' % f)

def ConfigurePortod(name, conf):
    path = '/etc/portod.conf.d/{}.conf'.format(name)
    if os.path.exists(path) and open(path).read() == conf:
        return
    if conf:
        if not os.path.exists('/etc/portod.conf.d'):
            os.mkdir('/etc/portod.conf.d', 0o775)
        open(path, 'w').write(conf)
    elif os.path.exists(path):
        os.unlink(path)
    ReloadPortod()

def ProcStatus(pid, key):
    for line in open("/proc/{}/status".format(pid)).readlines():
        k, v = line.split(None, 1)
        if k == key + ":":
            return v.strip()
    return None

def GetState(pid):
    if isinstance(pid, int):
        pid = str(pid)
    ss = open("/proc/" + pid + "/status").readlines()
    for s in ss:
        if s.find("State:") >= 0:
            return s.split()[1]
    return ""

def IsRunning(pid):
    try:
        os.kill(pid, 0)
        state = GetState(pid)
        return state != "Z" and state != "X"
    except:
        return False

def IsZombie(pid):
    return GetState(pid) == "Z"

def KillPid(pid, signal):
    os.kill(pid, signal)
    try:
        ctr = 0
        while ctr < 100:
            time.sleep(0.1)
            os.kill(pid, 0)
            ctr += 1

        raise BaseException("Too long waited for portod to stop")
    except OSError:
        pass

def GetMeminfo(tag):
    meminfo = open("/proc/meminfo", "r").readlines()
    for m in meminfo:
        if m.find(tag) >= 0:
            return int(m.split()[1]) * 1024

def get_kernel_maj_min():
    kver = re.match("([0-9])\.([0-9]{1,2})", platform.uname()[2]).groups()
    return (int(kver[0]), int(kver[1]))

if not os.environ.get('PORTO_TEST_NO_RESTART', None):
    CleanupConfigs()
    RestartPortod()
