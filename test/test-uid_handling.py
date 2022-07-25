#!/usr/bin/python

import porto
import pwd
import re
from test_common import *

#TMPDIR="/tmp/test-uid_handling"

AsAlice()

c = porto.Connection(timeout=30)

r = c.CreateWeakContainer("test")
r.SetProperty("porto_namespace", "")

v = c.CreateVolume(None, layers=["ubuntu-precise"])
v.Link(r.name)

#Check virt_mode == os user handling

r.SetProperty("virt_mode", "os")
r.SetProperty("root", v.path)
r.SetProperty("bind", "{} /portobin/ ro".format(portobin))
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=porto-bob")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "porto-bob"
assert r2.GetProperty("owner_user") == "porto-alice"
assert r2.GetProperty("user") == "porto-bob"
r2.Destroy()
r.Stop()

r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\"")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "root"
assert r2.GetProperty("owner_user") == "porto-alice"
assert r2.GetProperty("user") == "root"
r2.Destroy()
r.Stop()

#We shouldn't be allowed to create sibling with root user
r.SetProperty("command", "/portobin/portoctl run -W b command=\"id -u\" isolate=false user=root")
r.Start()
assert r.Wait() == "test"

assert r.GetProperty("exit_status") == "256"
r.Stop()
r.Destroy()

#Check virt_mode == app now

r = c.CreateWeakContainer("test")
r.SetProperty("porto_namespace", "")

v.Link(r.name)
r.SetProperty("root", v.path)
r.SetProperty("bind", "{} /portobin/ ro".format(portobin))

r.SetProperty("virt_mode", "app")
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\"")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "porto-alice"
assert r2.GetProperty("owner_user") == "porto-alice"
assert r2.GetProperty("user") == "porto-alice"
r2.Destroy()

r.Stop()
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=porto-bob")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "porto-bob"
assert r2.GetProperty("owner_user") == "porto-alice"
assert r2.GetProperty("user") == "porto-bob"
r2.Destroy()

r.Stop()
r.SetProperty("user", "porto-charlie")
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\"")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "porto-charlie"
assert r2.GetProperty("user") == "porto-charlie"
assert r2.GetProperty("owner_user") == "porto-alice"
r2.Destroy()
r.Stop()

#Tricky moment: subcontainer creation rights depends on owner_user
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=porto-alice")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "porto-alice"
assert r2.GetProperty("user") == "porto-alice"
r2.Destroy()
r.Stop()

r.SetProperty("command", "/portobin/portoctl run -W b command=\"id -u\" user=porto-bob")
r.Start()
assert r.Wait() == "test"

assert r.GetProperty("exit_status")
assert re.search("Permission", r.GetProperty("stderr")) is not None
r.Destroy()

#Check what if the owner will be root...

c.disconnect()
AsRoot()
c.connect()

#Usually porto-alice cannot run -W as porto-bob, but with owner_user == root solves everything

r = c.CreateWeakContainer("test")
r.SetProperty("porto_namespace", "")

r.SetProperty("root", v.path)
r.SetProperty("bind", "{} /portobin/ ro".format(portobin))
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=porto-bob")
r.SetProperty("user", "porto-alice")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "porto-bob"
assert r2.GetProperty("user") == "porto-bob"
r2.Destroy()
r.Stop()

r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=root")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("test/a")
assert r2.Wait() == "test/a"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "root"
assert r2.GetProperty("user") == "root"
r2.Destroy()
r.Stop()

#Now, we can create sibling with root user
r.SetProperty("command", "/portobin/portoctl run -W b command=\"id -u\" user=root")
r.Start()
assert r.Wait() == "test"

r2 = c.Find("b")
assert r2.Wait() == "b"
assert pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name == "root"
assert r2.GetProperty("user") == "root"
r2.Destroy()
r.Stop()

#Check if we can prevent creating root-created container create sibline with root user
r.SetProperty("enable_porto", "child-only")
r.Start()
assert r.Wait() == "test"

assert r.GetProperty("exit_status") == "256"
assert re.search("Permission", r.GetProperty("stderr"))

r.Destroy()
v.Unlink("/")

#Create volume for non-existing user
AsAlice()
c.connect()

v = c.CreateVolume(None, user="7255")
ExpectEq(os.stat(v.path).st_uid, 7255)
ExpectEq(os.stat(v.path).st_gid, alice_gid)
v.Unlink()

v = c.CreateVolume(None, group="7255")
ExpectEq(os.stat(v.path).st_uid, alice_uid)
ExpectEq(os.stat(v.path).st_gid, 7255)
v.Unlink()

v = c.CreateVolume(None, user="7255", group="7255")
ExpectEq(os.stat(v.path).st_uid, 7255)
ExpectEq(os.stat(v.path).st_gid, 7255)
v.Unlink()
