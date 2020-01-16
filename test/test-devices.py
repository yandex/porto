#!/usr/bin/python

import sys
import subprocess
import porto
from test_common import *

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
ExpectEq(a["exit_code"], "0")
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
assert b["state"] == "meta"
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

# ----------------------------------------
# Check reverting to default+extra devices after empty devices knob is given
def GetCgroupDevices(ct):
    with open("/sys/fs/cgroup/devices/porto%{}/devices.list".format(ct.name), "r") as dev_list_file:
        return set(dev_list_file.read().splitlines())

a = c.Run("a", weak=False, root_volume={"layers": ["ubuntu-precise"]})
default_devices = GetCgroupDevices(a)
a.Destroy()

ConfigurePortod('test-devices', """
container {
        extra_devices: "/dev/ram0 rwm;"
        }
""")
# "/dev/ram0 rwm" = "b 1:0 rwm"
def_extra_devices = default_devices.copy()
def_extra_devices.add("b 1:0 rwm")

# "/dev/ram1 rwm" = "b 1:1 rwm"
all_devices = def_extra_devices.copy()
all_devices.add("b 1:1 rwm")

# ----------------------------------------
# set extra device on container start
a = c.Run("a", weak=False, root_volume={"layers": ["ubuntu-precise"]}, devices="/dev/ram0 rwm")
ExpectEq(a.GetProperty("devices"), "/dev/ram0 rwm /dev/ram0 0666 root disk; "); # notice the trailing space
devices = GetCgroupDevices(a)
ExpectEq(devices, def_extra_devices)

a.SetProperty("devices", "");
ExpectEq(a.GetProperty("devices"), "");
devices = GetCgroupDevices(a)
ExpectEq(devices, def_extra_devices)

a.Destroy()

# ----------------------------------------
# set extra device explicitly
a = c.Run("a", weak=False, root_volume={"layers": ["ubuntu-precise"]}, devices="/dev/ram1 rwm")
ExpectEq(a.GetProperty("devices"), "/dev/ram1 rwm /dev/ram1 0660 root disk; "); # notice the trailing space
devices = GetCgroupDevices(a)
ExpectEq(devices, all_devices)

a.SetProperty("devices", "/dev/ram0 rwm");
ExpectEq(a.GetProperty("devices"), "/dev/ram1 - /dev/ram1 0660 root disk; /dev/ram0 rwm /dev/ram0 0666 root disk; "); # notice the trailing space
devices = GetCgroupDevices(a)
ExpectEq(devices, def_extra_devices)

a.SetProperty("devices", "");
ExpectEq(a.GetProperty("devices"), "");
devices = GetCgroupDevices(a)
ExpectEq(devices, def_extra_devices)

ConfigurePortod('test-devices', "")
a.Destroy()
# ----------------------------------------

# ----------------------------------------
# Check updating of devices (cgroups and nodes) in chroot after porto reload
os.mkdir("/tmp/test-devices")
os.mkdir("/tmp/test-devices/volume")
vol = c.CreateVolume("/tmp/test-devices/volume", layers=["ubuntu-precise"])
os.mkdir("/tmp/test-devices/volume/volume")
vol_vol = c.CreateVolume("/tmp/test-devices/volume/volume", layers=["ubuntu-precise"])

ConfigurePortod('test-devices', """
container {
        extra_devices: "/dev/ram0 rwm;"
        }
""")
a = c.Run("a", weak=False)
ab = c.Run("a/b", weak=False, root=vol.path)
devices = GetCgroupDevices(ab)

abc = c.Run("a/b/c", weak=False, root="/volume", command="ls dev")
abc.Wait()
ExpectEq(abc["exit_code"], "0")
ExpectEq(devices, GetCgroupDevices(abc))
abc.Destroy()

# ConfigurePortod also do porto reload
ConfigurePortod('test-devices', """
container {
        extra_devices: "/dev/ram0 rwm; /dev/ram1 rwm;"
        }
""")
devices.add("b 1:1 rwm")
ExpectEq(devices, GetCgroupDevices(ab))

abc = c.Run("a/b/c", weak=False, root="/volume", command="ls dev")
abc.Wait()
ExpectEq(abc["exit_code"], "0")
ExpectEq(devices, GetCgroupDevices(abc))

ConfigurePortod('test-devices', "")
a.Destroy()
c.DestroyVolume("/tmp/test-devices/volume")
os.rmdir("/tmp/test-devices/volume")
os.rmdir("/tmp/test-devices")
# ----------------------------------------

# Checking devices nodes + hierarchy
# No access, but device node should persist in child
a = None
try:
    a = c.Run("a", weak=False, root_volume={"layers": ["ubuntu-precise"]}, devices="/dev/ram0 rwm")
    ab = c.Run("a/b", wait=60, devices="/dev/ram0 -", command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectNe(ab["exit_code"], "0")
    ab.Destroy()
    ab = c.Run("a/b", wait=60, devices="/dev/ram0 -", command="stat /dev/ram0")
    ExpectEq(ab["exit_code"], "0")
    ab.Destroy()

    # Device node from parent should not be missed after dynamic update
    ab = c.Run("a/b", command="sleep infinity", devices="/dev/ram0 -")
    ab.SetProperty("devices", "/dev/ram0 -")

    # Child with restriction should not access devices, but still observe it
    abc = c.Run("a/b/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectNe(abc["exit_code"], "0")
    abc.Destroy()
    abc = c.Run("a/b/c", wait=60, command="stat /dev/ram0")
    ExpectEq(abc["exit_code"], "0")
    abc.Destroy()

    # Free child should observe same devices as designated in parent
    ac = c.Run("a/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectEq(ac["exit_code"], "0")
    ac.Destroy()
    ac = c.Run("a/c", wait=60, command="stat /dev/ram0")
    ExpectEq(ac["exit_code"], "0")
    ac.Destroy()
    ab.Destroy()

    # Now the same scenario, but after portod reload
    ab = c.Run("a/b", weak=False, command="sleep infinity", devices="/dev/ram0 -")
    ReloadPortod()
    abc = c.Run("a/b/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectNe(abc["exit_code"], "0")
    abc.Destroy()
    abc = c.Run("a/b/c", wait=60, command="stat /dev/ram0")
    ExpectEq(abc["exit_code"], "0")
    abc.Destroy()
    ac = c.Run("a/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectEq(ac["exit_code"], "0")
    ac.Destroy()
    ac = c.Run("a/c", wait=60, command="stat /dev/ram0")
    ExpectEq(ac["exit_code"], "0")
    ac.Destroy()
    ab.Destroy()

    # With chroot in child the device node can be safely deleted
    child_root = "%s/child" % a["root"]
    os.mkdir(child_root)
    v1 = c.CreateVolume(child_root, layers=["ubuntu-precise"])
    ab = c.Run("a/b", command="sleep infinity", root="/child")
    v1.Link("a/b")
    v1.Unlink("/")
    abc = c.Run("a/b/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectEq(abc["exit_code"], "0")
    abc.Destroy()
    ab.SetProperty("devices", "/dev/ram0 -")

    # We observe nothing in child with chroot
    abc = c.Run("a/b/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectNe(abc["exit_code"], "0")
    abc.Destroy()
    abc = c.Run("a/b/c", wait=60, command="stat /dev/ram0")
    ExpectNe(abc["exit_code"], "0")
    abc.Destroy()

    # Parent device node persists and accessible from non-chrooted child
    ac = c.Run("a/c", wait=60, command="dd if=/dev/ram0 of=/dev/null count=1")
    ExpectEq(ac["exit_code"], "0")
    ac.Destroy()
    ac = c.Run("a/c", wait=60, command="stat /dev/ram0")
    ExpectEq(ac["exit_code"], "0")
    ac.Destroy()
    ab.Destroy()
finally:
    if a:
        a.Destroy()

os.chmod("/dev/ram0", 0660)


try:
    subprocess.check_call(['gdb', '--version'])
except:
    print "Skipping gdb test"
    sys.exit(0)

def run_test_restore_vanished_pids_devices(*args, **kwargs):
    a = None
    try:
        a = c.Run("a", weak=False, root_volume={"layers": ["ubuntu-precise"]}, command="sleep 5", **kwargs)

        app_pid, _, portoinit_pid = a["_root_pid"].split(';')

        c.disconnect()

        # Prevent portoint from immediate exit
        subprocess.check_call(['gdb', '-p', portoinit_pid, './portoinit', '-ex', 'b _exit', '-ex', 'c', '-ex', 'signal SIGSTOP', '-ex', 'detach', '-ex', 'quit'])
        ReloadPortod()

        c.connect()
        ExpectNe(a["state"], "stopped")
        a.Destroy()
        a = None
    finally:
        c.connect()
        if a:
            a.Destroy()

run_test_restore_vanished_pids_devices()
run_test_restore_vanished_pids_devices(devices="/dev/ram0 rw")
