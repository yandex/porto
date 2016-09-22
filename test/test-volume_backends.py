#!/usr/bin/python

import os
import time
import array
import fcntl
import struct
import shutil
import tarfile
import subprocess

import porto
from test_common import *

DIR="/tmp/test-volumes"
PLACE="/place/porto_volumes"
c = None

def porto_reconnect(c):
    if c is not None:
        c.disconnect()

    return porto.Connection(timeout=600)

def get_quota_fs_projid(path):
    fmt = "IIII12x"
    arr = array.array('B')
    arr.fromstring("\x00" * struct.calcsize(fmt))
    fd = os.open(path , os.O_RDONLY | os.O_NOCTTY | os.O_NOFOLLOW |
                       os.O_NOATIME | os.O_NONBLOCK)
    fcntl.ioctl(fd, 0x801c581f, arr)
    projid = struct.unpack("IIII12x", arr.tostring())[3]
    os.close(fd)
    return projid

def cleanup_portod(c):
    for r in c.ListContainers():
        if r.name != "/":
            r.Destroy()

    for v in c.ListVolumes():
        v.Unlink()

def check_readonly(c, path, **args):
    args["read_only"] = "true"
    v = c.CreateVolume(path, **args)
    c.Create("test")
    c.SetProperty("test", "command", "bash -c \"echo 123 > " + v.path + "/123.txt\"")
    c.Start("test")
    c.Wait(["test"])
    assert c.GetProperty("test", "exit_status") == "256"
    c.Destroy("test")
    v.Unlink("/")

def check_place(c, path, **args):
    place_num = len(os.listdir(PLACE))
    vol_num = len(c.ListVolumes())

    v = c.CreateVolume(path, **args)
    place_volumes = os.listdir(PLACE)
    assert len(place_volumes) == place_num + 1
    assert len(c.ListVolumes()) == vol_num + 1

    os.stat(PLACE + "/" + place_volumes[0] + "/" + args["backend"])
    v.Unlink("/")

    assert len(os.listdir(PLACE)) == place_num
    assert len(c.ListVolumes()) == vol_num

def check_destroy_linked_non_root(c, path, **args):
    place_num = len(os.listdir(PLACE))
    vol_num = len(c.ListVolumes())

    v = c.CreateVolume(path, **args)

    place_volumes = os.listdir(PLACE)
    assert len(place_volumes) == place_num + 1
    assert len(c.ListVolumes()) == vol_num + 1

    r = c.Create("test")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("command", "echo 123")
    r.Start()
    r.Wait()
    r.Destroy()

    assert len(os.listdir(PLACE)) == place_num
    assert len(c.ListVolumes()) == vol_num

def check_mounted(c, path, **args):
    v = c.CreateVolume(path, **args)
    open(v.path + "/file.txt", "w").write("aabb")
    r = c.Create("test")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("command", "bash -c \"cat " + v.path + "/file.txt; echo -n 3210 > " +\
                  v.path + "/file.txt  \"")
    r.Start()
    r.Wait()
    assert r.GetProperty("stdout") == "aabb"
    if path is not None:
        assert open(path + "/file.txt", "r").read() == "3210"
    assert open(v.path + "/file.txt", "r").read() == "3210"
    path = v.path
    r.Destroy()
    if args["backend"] != "quota":
        assert Catch(os.stat, path + "/file.txt") == OSError
    else:
        os.remove(path + "/file.txt")

def check_space_limit(c, path, limits, cleanup=True, **args):
    args["space_limit"] = limits[0]
    v = c.CreateVolume(path, **args)
    r = c.Create("test")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("command", "dd if=/dev/zero of=" + v.path + "/file.zeroes " + \
                  "bs=1048576 count=" + limits[1])
    r.Start()
    r.Wait()

    assert os.stat(v.path + "/file.zeroes").st_size <= int(limits[2])
    if not cleanup:
        return (r,v)
    r.Destroy()

def check_tune_space_limit(c, path, limits=None, **args):
    if limits is None:
        limits = ("1M", "3", "1048576", "4M", "3145728")
    (r,v) = check_space_limit(c, path, limits, False, **args)
    r.Stop()
    os.remove(v.path + "/file.zeroes")
    v.Tune(space_limit=limits[3])
    r.Start()
    r.Wait()
    assert os.stat(v.path + "/file.zeroes").st_size == int(limits[4])
    r.Destroy()

def check_inode_limit(c, path, cleanup=True, **args):
    if "inode_limit" not in args:
        args["inode_limit"] = "16"
    v = c.CreateVolume(path, **args)
    r = c.Create("test")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("command", "bash -c \"for i in \$(seq 1 24); do touch " + v.path +\
                  "/\$i; done\"")
    r.Start()
    r.Wait()
    assert r.GetProperty("exit_status") == "256"
    assert len(os.listdir(v.path)) < 24
    if not cleanup:
        return (r, v)
    r.Destroy()

def check_tune_inode_limit(c, path, **args):
    (r, v) = check_inode_limit(c, path, False, **args)
    r.Stop()
    v.Tune(inode_limit="32")
    r.Start()
    r.Wait()
    assert r.GetProperty("exit_status") == "0"
    assert len(os.listdir(v.path)) >= 24
    r.Destroy()

def check_layers(c, path, cleanup=True, **args):
    args["layers"] = ["ubuntu-precise", "test-volumes"]
    v = c.CreateVolume(path, **args)
    r = c.Create("test")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("root", v.path)
    r.SetProperty("command", "cat /test_file.txt")
    r.Start()
    r.Wait()
    assert r.GetProperty("exit_status") == "0"
    assert r.GetProperty("stdout") == "1234567890"
    if not cleanup:
        return (r, v)
    r.Destroy()

def check_projid_is_set(c, path, **args):
    v = c.CreateVolume(path, **args)
    open(path + "/file.txt", "w").write("111")

    #Checking quota project id is set
    assert get_quota_fs_projid(DIR) == 0
    projid = get_quota_fs_projid(path)
    ino_id = os.stat(path).st_ino

    assert projid == ino_id + 2 ** 31
    assert get_quota_fs_projid(path + "/file.txt") == projid

    v.Unlink("/")

    assert get_quota_fs_projid(path) == 0
    assert get_quota_fs_projid(path + "/file.txt") == 0

    os.unlink(path + "/file.txt")

def backend_plain(c):
    args = dict()
    args["backend"] = "plain"

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)

    for path in [None, TMPDIR]:

        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        assert Catch(check_tune_space_limit, c, path, **args) ==\
                     porto.exceptions.InvalidProperty
        assert Catch(check_tune_inode_limit, c, path, **args) ==\
                     porto.exceptions.InvalidProperty
        check_layers(c, path, **args)

    os.rmdir(TMPDIR)


def backend_bind(c):
    args = dict()
    args["backend"] = "bind"

    TMPDIR = DIR + "/bind"
    os.mkdir(TMPDIR)

    assert Catch(check_mounted, c, None, **args) == porto.exceptions.InvalidProperty
    assert Catch(check_mounted, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    BIND_STORAGE_DIR = DIR + "/bind_storage"
    os.mkdir(BIND_STORAGE_DIR)
    args["storage"] = BIND_STORAGE_DIR

    check_readonly(c, TMPDIR, **args)
    check_destroy_linked_non_root(c, TMPDIR, **args)
    check_mounted(c, TMPDIR, **args)
    assert Catch(check_tune_space_limit, c, TMPDIR, **args) ==\
                 porto.exceptions.InvalidProperty
    assert Catch(check_tune_inode_limit, c, TMPDIR, **args) ==\
                 porto.exceptions.InvalidProperty
    assert Catch(check_layers, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    os.rmdir(TMPDIR)
    os.remove(BIND_STORAGE_DIR + "/file.txt")
    os.rmdir(BIND_STORAGE_DIR)

def backend_tmpfs(c):
    args = dict()
    args["backend"] = "tmpfs"
    TMPDIR = DIR + "/tmpfs"
    os.mkdir(TMPDIR)


    for path in [None, TMPDIR]:
        args["space_limit"] = "1M"
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        args["space_limit"] = "512M"
        check_layers(c, path, **args)

    os.rmdir(TMPDIR)

def backend_quota(c):
    args = dict()
    args["backend"] = "quota"
    TMPDIR = DIR + "/quota"
    os.mkdir(TMPDIR)

    assert Catch(check_mounted, c, None, **args) == porto.exceptions.InvalidProperty
    assert Catch(check_mounted, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")

    args["space_limit"] = "1M"
    check_mounted(c, TMPDIR, **args)
    assert Catch(check_readonly, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    check_place(c, TMPDIR, **args)
    check_destroy_linked_non_root(c, TMPDIR, **args)

    assert Catch(check_tune_space_limit, c, TMPDIR, **args) == AssertionError
    cleanup_portod(c)
    os.remove(TMPDIR + "/file.zeroes")

    os.chown(TMPDIR, alice_uid, alice_gid)
    SwitchUser("porto-alice", alice_uid, alice_gid)
    c = porto_reconnect(c)

    check_tune_space_limit(c, TMPDIR, **args)

    SwitchRoot()
    c = porto_reconnect(c)

    assert Catch(check_tune_inode_limit, c, TMPDIR, **args) == AssertionError
    cleanup_portod(c)
    for i in os.listdir(TMPDIR):
        os.remove(TMPDIR + "/" + i)

    SwitchUser("porto-alice", alice_uid, alice_gid)
    c = porto_reconnect(c)

    check_tune_inode_limit(c, TMPDIR, **args)

    assert Catch(check_layers, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    SwitchRoot()
    c = porto_reconnect(c)

    args["space_limit"] = "1M"
    check_projid_is_set(c, TMPDIR, **args)

    for i in os.listdir(TMPDIR):
        if int(i) <= 24:
            os.remove(TMPDIR+"/"+i)
    os.rmdir(TMPDIR)

def backend_native(c):
    args = dict()
    args["backend"] = "native"

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")
    os.chown(TMPDIR, alice_uid, alice_gid)

    for path in [None, TMPDIR]:
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        SwitchUser("porto-alice", alice_uid, alice_gid)
        c = porto_reconnect(c)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        SwitchRoot()
        c = porto_reconnect(c)
        check_layers(c, path, **args)

    os.rmdir(TMPDIR)

def backend_overlay(c):
    args = dict()
    args["backend"] = "overlay"
    args["layers"] = ["test-volumes"]

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)
    (alice_uid, alice_gid) = GetUidGidByUsername("porto-alice")
    os.chown(TMPDIR, alice_uid, alice_gid)

    for path in [None, TMPDIR]:

        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        SwitchUser("porto-alice", alice_uid, alice_gid)
        c = porto_reconnect(c)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        SwitchRoot()
        c = porto_reconnect(c)

        (r,v) = check_layers(c, path, False, **args)
        r.Stop()
        r.SetProperty("command", "bash -c \"echo -n 1234 > /test_file2.txt\"")
        r.Start()
        r.Wait()
        v.Export(DIR + "/upper.tar")
        r.Destroy()
        assert tarfile.open(name=DIR + "/upper.tar").extractfile("test_file2.txt").read() == "1234"
        os.remove(DIR + "/upper.tar")

    os.rmdir(TMPDIR)

def backend_loop(c):
    args = dict()
    args["backend"] = "loop"
    args["space_limit"] = "1M"

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)

    for path in [None, TMPDIR]:
        args["space_limit"] = "512M"
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)
        check_tune_space_limit(c, path, ("512M", "768", "536870912", "1024M", "805306368"), **args)
        args["space_limit"] = "1G"
        check_layers(c, path, **args)

    args["space_limit"] = "512M"
    v = c.CreateVolume(**args)
    args["storage"] = os.path.abspath(v.path + "/../loop/loop.img")
    assert Catch(c.CreateVolume, **args) == porto.exceptions.Busy
    v.Unlink("/")

    f = open(DIR + "/loop.img", "w")
    f.truncate(512 * 1048576)
    f.close()

    subprocess.check_call(["mkfs.ext4", "-F", "-q", DIR + "/loop.img"])
    args["storage"] = os.path.abspath(DIR + "/loop.img")
    v = c.CreateVolume(**args)
    assert Catch(c.CreateVolume, **args) == porto.exceptions.Busy
    v.Unlink("/")

    os.unlink(DIR + "/loop.img")
    os.rmdir(TMPDIR)

def backend_rbd():
    #Not implemented yet
    pass


if os.getuid() != 0:
    SwitchRoot()

Catch(shutil.rmtree, DIR)
os.mkdir(DIR)

c = porto_reconnect(c)
assert len(os.listdir(PLACE)) == 0
assert len(c.ListVolumes()) == 0

DUMMYLAYER = DIR + "/test_layer.tar"
open(DIR + "/test_file.txt", "w").write("1234567890")
t = tarfile.open(name=DUMMYLAYER, mode="w")
t.add(DIR + "/test_file.txt", arcname="test_file.txt")
t.close()
os.remove(DIR + "/test_file.txt")

Catch(c.RemoveLayer, "test-volumes")
c.ImportLayer("test-volumes", DUMMYLAYER)
os.unlink(DUMMYLAYER)

backend_plain(c)
backend_bind(c)
backend_tmpfs(c)
backend_quota(c)
backend_native(c)
backend_overlay(c)
backend_loop(c)

c.RemoveLayer("test-volumes")
assert len(c.ListVolumes()) == 0
assert len(os.listdir(PLACE)) == 0
os.rmdir(DIR)
