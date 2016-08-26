import sys
import os
import pwd
import grp

layers = dict()
layers["precise-base-porto"] = "/place/porto-test-layers/ubuntu-precise-base-porto.tar.xz"

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

portoctl = os.getcwd() + "/portoctl"

def GetUidGidByUsername(username):
    return (pwd.getpwnam(username).pw_uid, grp.getgrnam(username).gr_gid)
