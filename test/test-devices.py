#!/usr/bin/python

import porto
import subprocess
from test_common import *

subprocess.call(["modprobe", "brd"])
os.chmod("/dev/ram0", 0666)

AsAlice()

c = porto.Connection()

a = c.Run("a", wait=60, command="dd if=/dev/urandom of=/dev/null count=1", devices="/dev/urandom rw")
ExpectEq(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", wait=60, command="dd if=/dev/urandom of=/dev/null count=1", devices="/dev/urandom -")
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/urandom rw")
ExpectNe(a["exit_code"], "0")
a.Destroy()

# virt_mode=host

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", virt_mode="host")
ExpectEq(a["exit_code"], "0")
a.Destroy()

# disable controller

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", **{"controllers[devices]": "false"})
ExpectEq(a["exit_code"], "0")
a.Destroy()

# allow access

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 rw")
ExpectEq(a["exit_code"], "0")
a.Destroy()

# in chroot

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", root_volume={"layers": ["ubuntu-precise"]})
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 m", root_volume={"layers": ["ubuntu-precise"]})
ExpectNe(a["exit_code"], "0")
a.Destroy()

a = c.Run("a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 rw", root_volume={"layers": ["ubuntu-precise"]})
ExpectEq(a["exit_code"], "0")
a.Destroy()

s = c.Run("s")

m = c.Run("s/m", root_volume={"layers": ["ubuntu-precise"]})

a = c.Run("s/m/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectNe(a["exit_code"], "0")
a.Destroy()

s["devices"] = "/dev/ram0 rw"

a = c.Run("s/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", root_volume={"layers": ["ubuntu-precise"]})
ExpectEq(a["exit_code"], "0")
a.Destroy()

a = c.Run("s/m/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = "/dev/ram0 rw"

a = c.Run("s/m/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectEq(a["exit_code"], "0")
a.Destroy()

a = c.Run("s/m/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1", devices="/dev/ram0 -")
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = "/dev/ram0 m"

a = c.Run("s/m/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectNe(a["exit_code"], "0")
a.Destroy()

m["devices"] = ""

a = c.Run("s/m/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectNe(a["exit_code"], "0")
a.Destroy()

m.Destroy()
s.Destroy()

m = c.Run("m", devices="/dev/ram0 rw")
b = c.Run("m/b", root_volume={"layers": ["ubuntu-precise"]}, devices="/dev/ram0 -")
b["devices"] = "/dev/ram0 rw"
a = c.Run("m/b/a", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
ExpectEq(a["exit_code"], "0")
a.Destroy()
b.Destroy()
m.Destroy()


ExpectEq(Catch(c.Run, "m", devices="/dev//ram0 rw"), porto.exceptions.InvalidValue)

ExpectEq(Catch(c.Run, "m", devices="/dev/../ram0 rw"), porto.exceptions.InvalidValue)

ExpectEq(Catch(c.Run, "m", devices="/tmp/ram0 rw"), porto.exceptions.InvalidValue)

ExpectEq(Catch(c.Run, "m", devices="/dev/__missing__ rw"), porto.exceptions.DeviceNotFound)

m = c.Run("m", devices="/dev/__missing__ rw?")
ExpectEq(m['devices'], "")
m.Destroy()

ExpectEq(Catch(c.Run, "m", devices="/dev/ram1 rw"), porto.exceptions.Permission)

m = c.Run("m")
ExpectEq(Catch(c.Run, "m/a", devices="/dev/ram0 rw"), porto.exceptions.Permission)
m.Destroy()

AsRoot()

a = c.Run("a", weak=False, **{"controllers[devices]": "true"})
b = c.Run("a/b", weak=False, **{"controllers[devices]": "true"})
ReloadPortod()
ExpectEq(b["state"], "meta")
b.Destroy()
a.Destroy()

a = c.Run("a", weak=False, root_volume={"layers": ["ubuntu-precise"]}, devices="/dev/ram0 rw")
host_dev = os.stat("/dev/ram0")
ct_dev = os.stat("/proc/{}/root/dev/ram0".format(a['root_pid']))
ExpectEq(host_dev.st_mode, ct_dev.st_mode)
ExpectEq(host_dev.st_rdev, ct_dev.st_rdev)
ExpectEq(host_dev.st_uid, ct_dev.st_uid)
ExpectEq(host_dev.st_gid, ct_dev.st_gid)
ReloadPortod()
ct_dev = os.stat("/proc/{}/root/dev/ram0".format(a['root_pid']))
ExpectEq(host_dev.st_mode, ct_dev.st_mode)
ExpectEq(host_dev.st_rdev, ct_dev.st_rdev)
ExpectEq(host_dev.st_uid, ct_dev.st_uid)
ExpectEq(host_dev.st_gid, ct_dev.st_gid)
b = c.Run("a/b", command="chmod 421 /dev/ram0", wait=60)
b.Destroy()
b = c.Run("a/b", command="chown nobody /dev/ram0", wait=60)
ct_dev = os.stat("/proc/{}/root/dev/ram0".format(a['root_pid']))
ExpectNe(host_dev.st_mode, ct_dev.st_mode)
ReloadPortod()
ct_dev = os.stat("/proc/{}/root/dev/ram0".format(a['root_pid']))
ExpectEq(host_dev.st_mode, ct_dev.st_mode)
ExpectEq(host_dev.st_rdev, ct_dev.st_rdev)
ExpectEq(host_dev.st_uid, ct_dev.st_uid)
ExpectEq(host_dev.st_gid, ct_dev.st_gid)
a.Destroy()

os.chmod("/dev/ram0", 0660)
