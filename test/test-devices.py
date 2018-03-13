#!/usr/bin/python

import porto
from test_common import *

os.chmod("/dev/ram0", 0666)

AsAlice()

c = porto.Connection()

a = c.Run("a", command="dd if=/dev/urandom of=/dev/null count=1", devices="/dev/random rw")
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

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", root_volume={"layers": ["ubuntu-precise"]})
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 rw", root_volume={"layers": ["ubuntu-precise"]})
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

m = c.Run("m", root_volume={"layers": ["ubuntu-precise"]})

a = c.Run("m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = "/dev/ram0 rw"

a = c.Run("m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectEq(a["exit_code"], "0")
a.Destroy()

m["devices"] = "/dev/ram0 -"

a = c.Run("m/a", command="dd if=/dev/ram0 of=/dev/null count=1")
a.Wait()
ExpectNe(a["exit_code"], "0")
a.Destroy()

m.Destroy()

AsRoot()

os.chmod("/dev/ram0", 0660)
