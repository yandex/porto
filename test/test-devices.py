#!/usr/bin/python

import porto
from test_common import *

os.chmod("/dev/ram0", 0666)

AsAlice()

c = porto.Connection()

a = c.Run("a", command="dd if=/dev/urandom of=/dev/null count=1", devices="/dev/urandom rw")
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", command="dd if=/dev/urandom of=/dev/null count=1", devices="/dev/urandom -")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/urandom rw")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

# disable controller

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", **{"controllers[devices]": "false"})
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

# allow access

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 rw")
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

# in chroot

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", root_volume={"layers": ["ubuntu-precise"]})
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 m", root_volume={"layers": ["ubuntu-precise"]})
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 rw", root_volume={"layers": ["ubuntu-precise"]})
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

s = c.Run("s")

m = c.Run("s/m", root_volume={"layers": ["ubuntu-precise"]})

a = c.Run("s/m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

s["devices"] = "/dev/ram0 rw"

a = c.Run("s/a", command="dd if=/dev/ram0 of=/dev/null count=1", root_volume={"layers": ["ubuntu-precise"]})
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

a = c.Run("s/m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = "/dev/ram0 rw"

a = c.Run("s/m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

a = c.Run("s/m/a", command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 -")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = "/dev/ram0 m"

a = c.Run("s/m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = ""

a = c.Run("s/m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

m.Destroy()
s.Destroy()

m = c.Run("m", devices="/dev/ram0 rw")
b = c.Run("m/b", root_volume={"layers": ["ubuntu-precise"]}, devices="/dev/ram0 -")
b["devices"] = "/dev/ram0 rw"
a = c.Run("m/b/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()
b.Destroy()
m.Destroy()

AsRoot()

os.chmod("/dev/ram0", 0660)
