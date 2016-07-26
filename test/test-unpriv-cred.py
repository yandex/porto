import porto
import pwd
import grp
import sys
import os

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
    os.setuid(0)
    os.setgid(0)

alice_uid=pwd.getpwnam("porto-alice").pw_uid
alice_gid=grp.getgrnam("porto-alice").gr_gid
bob_uid=pwd.getpwnam("porto-bob").pw_uid
bob_gid=grp.getgrnam("porto-bob").gr_gid
charlie_uid=pwd.getpwnam("porto-charlie").pw_uid
charlie_gid=grp.getgrnam("porto-charlie").gr_gid
david_uid=pwd.getpwnam("porto-david").pw_uid
david_gid=grp.getgrnam("porto-david").gr_gid
porto_gid=grp.getgrnam("porto").gr_gid

Case="Case 1: charlie sets alice and alice-group from porto-containers"

SwitchUser("porto-charlie", charlie_uid, charlie_gid)
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("user", "porto-alice")
r.SetProperty("group", "porto-alice-group")
r.SetProperty("command", "ls")
r.Start()
assert r.Wait() == "test"
assert r.GetProperty("user") == "porto-alice"
print "%s OK!" %(Case)
r.Destroy()
SwitchRoot()

Case="Case 2: bob sets david from bob-containers"

SwitchUser("porto-bob", bob_uid, bob_gid)
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("user", "porto-david")
r.SetProperty("command", "ls")
r.Start()
assert r.Wait() == "test"
assert r.GetProperty("user") == "porto-david"
print "%s OK!" %(Case)
r.Destroy()
SwitchRoot()

Case="Case 3: bob creates and starts under himself, bob (not in *-containers)"

SwitchUser("porto-bob", bob_uid, bob_gid)
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("command", "ls")
r.Start()
assert r.Wait() == "test"
assert r.GetProperty("user") == "porto-bob"
print "%s OK!" %(Case)
r.Destroy()
SwitchRoot()

Case="Case 4: charlie sets bob (not in *-containers), catching exception"

SwitchUser("porto-charlie", charlie_uid, charlie_gid)
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("command", "ls")
assert Catch(r.SetProperty, "user", "porto-bob") == porto.exceptions.PermissionError
r.Destroy()
print "%s OK!" %(Case)
SwitchRoot()

Case="Case 5: bob sets charlie and alice-group (charlie not in alice-group), catching exception"

SwitchUser("porto-bob", bob_uid, bob_gid)
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("user", "porto-charlie")
assert Catch(r.SetProperty, "group", "porto-alice-group") == porto.exceptions.PermissionError
print "%s OK!" %(Case)
r.Destroy()
SwitchRoot()

Case="Case 6: root sets bob and alice-group (bob not in alice-group), root can do everything"
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("user", "porto-bob")
r.SetProperty("group", "porto-alice-group")
r.SetProperty("command", "ls")
r.Start()
assert r.Wait() == "test"
assert r.GetProperty("user") == "porto-bob"
assert r.GetProperty("group") == "porto-alice-group"
print "%s OK!" %(Case)
r.Destroy()

Case="Case 7: alice starts container created by bob with uid of david (david not in porto, porto-containers and porto-alice-containers), catching exception"

SwitchUser("porto-bob", bob_uid, bob_gid)
c = porto.Connection()
c.connect()
r = c.Create("test")
r.SetProperty("command", "ls")
r.SetProperty("user", "porto-david")
r.SetProperty("group", "porto-bob-containers")
SwitchRoot()
SwitchUser("porto-alice", alice_uid, alice_gid)
c = porto.Connection()
c.connect()
r = c.Find("test")
assert Catch(r.Start) == porto.exceptions.PermissionError
SwitchRoot()
SwitchUser("porto-bob", bob_uid, bob_gid)
c = porto.Connection()
c.connect()
r = c.Find("test")
r.Start()
assert r.Wait() == "test"
assert r.GetProperty("user") == "porto-david"
assert r.GetProperty("group") == "porto-bob-containers"
r.Destroy()
print "%s OK!" %(Case)
SwitchRoot()
