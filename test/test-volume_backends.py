#!/usr/bin/python

import os
import time
import array
import fcntl
import struct
import shutil
import tarfile
import subprocess
import traceback

import porto
from test_common import *

DIR="/tmp/test-volumes"
PLACE="/place/porto_volumes"
DUMMYLAYER = DIR + "/test_layer.tar"
c = None

def DumpObjectState(r, keys):
    for k in keys:
        try:
            value = r.GetProperty(k)
            try:
                value = value.rstrip()
            except:
                pass
        except:
            value = "n/a"

        print "{} : \"{}\"".format(k, value)

    print ""

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

    # FIXME keep_project_quota_id
    assert get_quota_fs_projid(path) == projid
    assert get_quota_fs_projid(path + "/file.txt") == projid

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
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)

    assert Catch(check_layers, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    os.rmdir(TMPDIR)

def backend_quota(c):
    args = dict()
    args["backend"] = "quota"
    TMPDIR = DIR + "/quota"
    os.mkdir(TMPDIR)

    assert Catch(check_mounted, c, None, **args) == porto.exceptions.InvalidProperty
    assert Catch(check_mounted, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    args["space_limit"] = "1M"
    check_mounted(c, TMPDIR, **args)
    assert Catch(check_readonly, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    check_destroy_linked_non_root(c, TMPDIR, **args)

    assert Catch(check_tune_space_limit, c, TMPDIR, **args) == AssertionError
    cleanup_portod(c)
    os.remove(TMPDIR + "/file.zeroes")

    os.chown(TMPDIR, alice_uid, alice_gid)
    AsAlice()
    c = porto_reconnect(c)

    check_tune_space_limit(c, TMPDIR, **args)

    AsRoot()
    c = porto_reconnect(c)

    assert Catch(check_tune_inode_limit, c, TMPDIR, **args) == AssertionError
    cleanup_portod(c)
    for i in os.listdir(TMPDIR):
        os.remove(TMPDIR + "/" + i)

    AsAlice()
    c = porto_reconnect(c)

    check_tune_inode_limit(c, TMPDIR, **args)

    assert Catch(check_layers, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    AsRoot()
    c = porto_reconnect(c)

    args["space_limit"] = "1M"
    check_projid_is_set(c, TMPDIR, **args)

    for i in os.listdir(TMPDIR):
        if int(i) <= 24:
            os.remove(TMPDIR+"/"+i)
    os.rmdir(TMPDIR)

    # do not set FS_XFLAG_PROJINHERIT for files
    v = c.CreateVolume()
    with open(os.path.join(v.path, 'f'), 'w') as _:
        pass
    v2 = c.CreateVolume(storage=v.path, space_limit='1M')
    v2.Unlink()
    v.Unlink()

def backend_native(c):
    args = dict()
    args["backend"] = "native"

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)
    os.chown(TMPDIR, alice_uid, alice_gid)

    for path in [None, TMPDIR]:
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        AsAlice()
        c = porto_reconnect(c)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        AsRoot()
        c = porto_reconnect(c)
        check_layers(c, path, **args)

    os.rmdir(TMPDIR)

def backend_overlay(c):
    def copyup_quota(dest):
        ALAYER = TMPDIR + "/a_layer.tar"

        f1 = os.tmpfile()
        fzero = open("/dev/zero", "rb")
        f1.write("1" * (32 * 1048576))
        size1 = os.fstat(f1.fileno()).st_size
        f1.seek(0)

        f2 = os.tmpfile()
        f2.write("2" * (32 * 1048576))
        size2 = os.fstat(f2.fileno()).st_size
        f2.seek(0)

        t = tarfile.open(name=ALAYER, mode="w")
        t.addfile(t.gettarinfo(arcname="a1", fileobj=f1), fileobj = f1)
        t.addfile(t.gettarinfo(arcname="a2", fileobj=f2), fileobj = f2)
        t.close()
        f1.close()
        f2.close()

        c.ImportLayer("a_layer", ALAYER)
        os.unlink(ALAYER)
        space_limit = (size1 + size2) * 3 / 4
        v = c.CreateVolume(dest, layers=["a_layer"], space_limit=str(space_limit))
        r = c.Create("a")
        r.SetProperty("command", "bash -c \'echo 123 >> {}/a1\'".format(v.path))
        r.Start()
        r.Wait()
        assert r.GetProperty("exit_status") == "0"
        assert int(v.GetProperty("space_used")) <= int(v.GetProperty("space_limit"))

        r.Stop()
        r.SetProperty("command", "bash -c \'echo 456 >> {}/a2 || true\'".format(v.path))
        r.Start()
        r.Wait()
        assert r.GetProperty("exit_status") == "0"
        assert int(v.GetProperty("space_used")) <= int(v.GetProperty("space_limit"))
        assert os.statvfs(v.path).f_bfree != 0

        r.Destroy()
        v.Unlink()
        c.RemoveLayer("a_layer", async=True)

    def opaque_xattr(dest):
        DLAYER = TMPDIR + "/d_layer.tar"

        os.mkdir(TMPDIR + "/d_dir")

        t = tarfile.open(name=DLAYER, mode="w")
        t.add(TMPDIR + "/d_dir", arcname="d1")
        t.add(TMPDIR + "/d_dir", arcname="d2")

        f = os.tmpfile()

        f.write("a1")
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d1/a1", fileobj=f), fileobj=f)
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d2/a1", fileobj=f), fileobj=f)

        f.seek(0)
        f.write("a2")
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d1/a2", fileobj=f), fileobj=f)
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d2/a2", fileobj=f), fileobj=f)

        t.close()
        f.close()
        os.rmdir(TMPDIR + "/d_dir")

        c.ImportLayer("d_layer", DLAYER)
        os.unlink(DLAYER)

        v = c.CreateVolume(dest, layers=["d_layer"])

        assert os.path.exists(v.path + "/d1")
        assert os.path.exists(v.path + "/d1/a1")
        assert os.path.exists(v.path + "/d1/a2")
        assert os.path.exists(v.path + "/d2")
        assert os.path.exists(v.path + "/d2/a1")
        assert os.path.exists(v.path + "/d2/a2")

        os.unlink(v.path + "/d2/a1")
        os.unlink(v.path + "/d2/a2")
        os.rmdir(v.path + "/d2")

        os.mkdir(v.path + "/d2")
        open(v.path + "/d2/a3", "w").write("a3")

        c.ExportLayer(v.path, DLAYER)
        c.ImportLayer("d_removed_layer", DLAYER)
        v.Unlink()

        v = c.CreateVolume(dest, layers=["d_removed_layer", "d_layer"])

        assert os.path.exists(v.path + "/d1")
        assert os.path.exists(v.path + "/d1/a1")
        assert os.path.exists(v.path + "/d1/a2")
        assert os.path.exists(v.path + "/d2")
        assert os.path.exists(v.path + "/d2/a3")
        assert open(v.path + "/d2/a3", "r").read() == "a3"
        try:
            assert not os.path.exists(v.path + "/d2/a1")
            assert not os.path.exists(v.path + "/d2/a2")
        except AssertionError:
            #FIXME: remove when tar --xargs wiil be used
            print "Directory opaqueness is lost as expected"
            pass

        v.Unlink()
        c.RemoveLayer("d_removed_layer")
        c.RemoveLayer("d_layer")

    args = dict()
    args["backend"] = "overlay"
    args["layers"] = ["test-volumes"]

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)
    os.chown(TMPDIR, alice_uid, alice_gid)

    for path in [None, TMPDIR]:
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        AsAlice()
        c = porto_reconnect(c)
        copyup_quota(path)
        opaque_xattr(path)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        AsRoot()
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

AsRoot()

Catch(shutil.rmtree, DIR)
os.mkdir(DIR)

c = porto_reconnect(c)

def TestBody(c):
    assert len(os.listdir(PLACE)) == 0
    assert len(c.ListVolumes()) == 0

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

ret = 0

try:
    TestBody(c)

except BaseException as e:
    print traceback.format_exc()
    ret = 1

AsRoot()
c = porto_reconnect(c)

if ret > 0:
    print "Dumping test state:\n"

    for r in c.ListContainers():
        print "name : \"{}\"".format(r.name)
        DumpObjectState(r, [ "command", "exit_status", "stdout", "stderr" ])

    for v in c.ListVolumes():
        print "path : \"{}\"".format(v.path)
        DumpObjectState(v, [ "backend", "place",
                             "space_limit", "space_guarantee", "space_used",
                             "inode_limit", "inode_guarantee", "inode_used",
                             "creator", "owner_user", "owner_group",
                             "storage" ])

for r in c.ListContainers():
    Catch(r.Destroy)

for v in c.ListVolumes():
    Catch(v.Unlink)

for l in ["test-volumes", DUMMYLAYER, "a_layer", "d_layer", "d_removed_layer"]:
    Catch(c.RemoveLayer, l)

if os.path.exists(DIR):
    Catch(shutil.rmtree, DIR)

sys.exit(ret)
