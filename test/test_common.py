import sys
import os
import pwd
import grp
import time
import platform
import re

def Catch(func, *args, **kwargs):
    try:
        func(*args, **kwargs)
    except:
        return sys.exc_info()[0]
    return None

def SwitchUser(username, uid, gid):
    os.initgroups(username, gid)
    os.setresgid(gid,gid,0)
    os.setresuid(uid,uid,0)

def SwitchRoot():
    (ruid, euid, suid) = os.getresuid()
    if suid == 0:
        os.setuid(0)
        os.setgid(0)
    else:
        raise Exception("Cannot switch on root user")

def DropPrivileges():
    if os.getuid() == 0:
        SwitchUser("porto-alice", pwd.getpwnam("porto-alice").pw_uid,
                   grp.getgrnam("porto-alice").gr_gid)

def GetUidGidByUsername(username):
    return (pwd.getpwnam(username).pw_uid, grp.getgrnam(username).gr_gid)

def GetSlavePid():
    pid = int(open("/run/portod.pid").read())
    open("/proc/" + str(pid) + "/status").readline().index("portod-slave")
    return pid

def GetMasterPid():
    pid = int(open("/run/portoloop.pid").read())
    open("/proc/" + str(pid) + "/status").readline().index("portod")
    return pid

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
    kver = re.match("([0-9])\.([0-9])", platform.uname()[2]).groups()
    return (int(kver[0]), int(kver[1]))

def DumpObjectState(r, keys):
    for k in keys:
        try:
            value = r.GetProperty(k)
            try:
                value = value.rstrip()
            except:
                pass
        except:
            value = "n/a"

        print "{} : \"{}\"".format(k, value)

    print ""

portosrc = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
portobin = os.getcwd()
portoctl = portobin + "/portoctl"
portod = os.path.abspath(portobin + "/portod")
portotest = portobin + "/portotest"
