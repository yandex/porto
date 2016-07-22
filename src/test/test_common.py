import sys
import os
import pwd
import grp

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
    if os.getresuid().suid == 0:
        os.setuid(0)
        os.setgid(0)

def DropPrivileges():
    if os.getuid() == 0:
        SwitchUser("porto-alice", pwd.getpwnam("porto-alice").pw_uid,
                   grp.getgrnam("porto-alice").gr_gid)
