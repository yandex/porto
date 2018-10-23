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
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "porto-bob")
ExpectEq(r2.GetProperty("owner_user"), "porto-alice")
ExpectEq(r2.GetProperty("user"), "porto-bob")
r2.Destroy()
r.Stop()

r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\"")
r.Start()
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "root")
ExpectEq(r2.GetProperty("owner_user"), "porto-alice")
ExpectEq(r2.GetProperty("user"), "root")
r2.Destroy()
r.Stop()

#We shouldn't be allowed to create sibling with root user
r.SetProperty("command", "/portobin/portoctl run -W b command=\"id -u\" isolate=false user=root")
r.Start()
ExpectEq(r.Wait(), "test")

ExpectEq(r.GetProperty("exit_status"), "256")
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
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "porto-alice")
ExpectEq(r2.GetProperty("owner_user"), "porto-alice")
ExpectEq(r2.GetProperty("user"), "porto-alice")
r2.Destroy()

r.Stop()
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=porto-bob")
r.Start()
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "porto-bob")
ExpectEq(r2.GetProperty("owner_user"), "porto-alice")
ExpectEq(r2.GetProperty("user"), "porto-bob")
r2.Destroy()

r.Stop()
r.SetProperty("user", "porto-charlie")
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\"")
r.Start()
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "porto-charlie")
ExpectEq(r2.GetProperty("user"), "porto-charlie")
ExpectEq(r2.GetProperty("owner_user"), "porto-alice")
r2.Destroy()
r.Stop()

#Tricky moment: subcontainer creation rights depends on owner_user
r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=porto-alice")
r.Start()
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "porto-alice")
ExpectEq(r2.GetProperty("user"), "porto-alice")
r2.Destroy()
r.Stop()

r.SetProperty("command", "/portobin/portoctl run -W b command=\"id -u\" user=porto-bob")
r.Start()
ExpectEq(r.Wait(), "test")

Expect(r.GetProperty("exit_status"))
Expect(re.search("Permission", r.GetProperty("stderr")) is not None)
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
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "porto-bob")
ExpectEq(r2.GetProperty("user"), "porto-bob")
r2.Destroy()
r.Stop()

r.SetProperty("command", "/portobin/portoctl run -W self/a command=\"id -u\" user=root")
r.Start()
ExpectEq(r.Wait(), "test")

r2 = c.Find("test/a")
ExpectEq(r2.Wait(), "test/a")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "root")
ExpectEq(r2.GetProperty("user"), "root")
r2.Destroy()
r.Stop()

#Now, we can create sibling with root user
r.SetProperty("command", "/portobin/portoctl run -W b command=\"id -u\" user=root")
r.Start()
ExpectEq(r.Wait(), "test")

r2 = c.Find("b")
ExpectEq(r2.Wait(), "b")
ExpectEq(pwd.getpwuid(int(r2.GetProperty("stdout"))).pw_name, "root")
ExpectEq(r2.GetProperty("user"), "root")
r2.Destroy()
r.Stop()

#Check if we can prevent creating root-created container create sibline with root user
r.SetProperty("enable_porto", "child-only")
r.Start()
ExpectEq(r.Wait(), "test")

ExpectEq(r.GetProperty("exit_status"), "256")
Expect(re.search("Permission", r.GetProperty("stderr")))

r.Destroy()
v.Unlink("/")

#Create volume for non-existing user
AsAlice()
c.connect()

v = c.CreateVolume(None, user="999")
ExpectEq(os.stat(v.path).st_uid, 999)
ExpectEq(os.stat(v.path).st_gid, alice_gid)
v.Unlink()

v = c.CreateVolume(None, group="999")
ExpectEq(os.stat(v.path).st_uid, alice_uid)
ExpectEq(os.stat(v.path).st_gid, 999)
v.Unlink()

v = c.CreateVolume(None, user="999", group="999")
ExpectEq(os.stat(v.path).st_uid, 999)
ExpectEq(os.stat(v.path).st_gid, 999)
v.Unlink()
