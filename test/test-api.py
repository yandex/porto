from test_common import *
from threading import Thread

import sys
import os
import porto
import time
import subprocess

AsAlice()

c = porto.Connection()

assert c.connected() == False
assert c.nr_connects() == 0

c.connect()

assert c.connected() == True
assert c.nr_connects() == 1

c.List()
c.Plist()
c.Dlist()
c.Vlist()
c.Version()

c.ContainerProperties()
c.VolumeProperties()

c.ListContainers()
c.ListVolumes()
c.ListLayers()
c.ListStorages()
c.ListMetaStorages()
c.GetVolumes()
c.FindLabel("TEST.test")

c.ListStorage()

r = c.Find("/")
r.GetData('cpu_usage')

prefix = "test-api.py-"
container_name = prefix + "a"
layer_name = prefix + "layer"
volume_private = prefix + "volume"
volume_size = 256*(2**20)
volume_size_eps = 40*(2**20) # loop/ext4: 5% reserve + 256 byte inode per 4k of data
volume_path = "/tmp/" + prefix + "layer"
tarball_path = "/tmp/" + prefix + "layer.tgz"
broken_tarball_path = "/tmp/" + prefix + "broken_layer.tgz"
broken_tarball_file1 = "/tmp/" + prefix + "broken1"
broken_tarball_file2 = "/tmp/" + prefix + "broken2"
storage_tarball_path = "/tmp/" + prefix + "storage.tgz"
storage_name = prefix + "volume_storage"
meta_storage_name = prefix + "meta_storage"
storage_in_meta = meta_storage_name + "/storage"
layer_in_meta = meta_storage_name + "/layer"


# CLEANUP

if not Catch(c.Find, container_name):
    c.Destroy(container_name)

if not Catch(c.FindVolume, volume_path):
    c.DestroyVolume(volume_path)

if os.access(volume_path, os.F_OK):
    os.rmdir(volume_path)

for v in c.ListVolumes():
    if v.GetProperties().get("private") == volume_private:
        c.DestroyVolume(v.path)

if not Catch(c.FindLayer, layer_name):
    c.RemoveLayer(layer_name)

if os.access(tarball_path, os.F_OK):
    os.unlink(tarball_path)

if os.access(storage_tarball_path, os.F_OK):
    os.unlink(storage_tarball_path)

for l in c.ListLayers():
    if l.name == layer_in_meta:
        l.Remove()

for st in c.ListStorages():
    if st.name == storage_in_meta:
        st.Remove()

for ms in c.ListMetaStorages():
    if ms.name == meta_storage_name:
        ms.Remove()


# CONTAINERS

assert Catch(c.Find, container_name) == porto.exceptions.ContainerDoesNotExist
assert not container_name in c.List()

a = c.Create(container_name)
assert a.name == container_name
assert c.Find(container_name).name == container_name
assert container_name in c.List()

time.sleep(2)
creation_time = a["creation_time"]
assert len(creation_time) != 0
AsRoot()
ReloadPortod()
AsAlice()
assert creation_time == a["creation_time"]

assert a["state"] == "stopped"
assert a.GetData("state") == "stopped"
a.SetProperty("command", "false")
assert a.GetProperty("command") == False

a.Set(command="/bin/true", private="test")
assert a.Get(["command", "state", "private"]) == {"command": "/bin/true", "state": "stopped", "private": "test"}

a.Start()
assert a.Wait() == a.name
assert a.GetData("state") == "dead"
assert a.GetData("exit_status") == "0"

assert c.Wait(['*']) == a.name

a.Stop()
assert a.GetData("state") == "stopped"

a.SetProperty("command", "sleep 60")
a.Start()
assert a.GetData("state") == "running"

a.Pause()
assert a.GetData("state") == "paused"

a.Resume()
assert a.GetData("state") == "running"

a.Kill(9)
assert a.Wait() == a.name
assert a.GetData("state") == "dead"
assert a.GetData("exit_status") == "9"

a.Stop()
assert a.GetData("state") == "stopped"

a.SetProperty("command", "echo test")

a.Start()
assert a.Wait() == a.name
assert a.GetData("exit_status") == "0"
assert a.GetData("stdout") == "test\n"
a.Stop()

a.SetProperty("command_argv", "echo\ttest2")
assert a.GetProperty("command_argv") == "echo\ttest2"
a.Start()
assert a.Wait() == a.name
assert a.GetData("exit_status") == "0"
assert a.GetData("stdout") == "test2\n"
a.Stop()

a.SetProperty("command_argv", "")
a.SetProperty("command_argv[0]", "echo")
a.SetProperty("command_argv[5]", "test3")
a.Start()
assert a.Wait() == a.name
assert a.GetData("exit_status") == "0"
assert a.GetData("stdout") == "    test3\n"

c.Destroy(a)

assert Catch(c.Find, container_name) == porto.exceptions.ContainerDoesNotExist
assert not container_name in c.List()

a = c.Run(container_name, command="sleep 5", private_value=volume_private)
assert a["command"] == "sleep 5"
assert a["private"] == volume_private
a.Destroy()


# LAYERS
AsRoot()

ConfigurePortod('test-api', """
volumes {
    fs_stat_update_interval_ms: 1000
}
""")
AsAlice()

c.ListVolumes()
v = c.CreateVolume(private=volume_private)
v.GetProperties()
v.Tune()
f = open(v.path + "/file", 'w')
f.write("test")
f.close()

v.Export(tarball_path)
l = c.ImportLayer(layer_name, tarball_path)
assert l.name == layer_name
assert c.FindLayer(layer_name).name == layer_name

# make broken layer
subprocess.check_call(['dd','if=/dev/urandom','of=' + broken_tarball_file1,'bs=2M','count=1'])
subprocess.check_call(['dd','if=/dev/urandom','of=' + broken_tarball_file2,'bs=2M','count=1'])
subprocess.check_call(['tar', '-czf', broken_tarball_path, broken_tarball_file1, broken_tarball_file2])
subprocess.check_call(['truncate', '-s' '1536K', broken_tarball_path]) # file.truncate corrupt tar header
ExpectEq(porto.exceptions.Unknown, Catch(c.ImportLayer, "abc", broken_tarball_path))
ExpectEq(porto.exceptions.HelperFatalError, Catch(c.ImportLayer, "abc", broken_tarball_path, verbose_error=True))

assert l.GetPrivate() == ""
l.SetPrivate("123654")
assert l.GetPrivate() == "123654"
l.SetPrivate("AbC")
assert l.GetPrivate() == "AbC"

l.Update()
assert l.owner_user == "porto-alice"
assert l.owner_group == "porto-alice"
assert l.last_usage >= 0
assert l.private_value == "AbC"

assert Catch(c.GetLayerPrivate, "my1980") == porto.exceptions.LayerNotFound
assert Catch(c.SetLayerPrivate, "my1980", "my1980") == porto.exceptions.LayerNotFound

assert Catch(c.CreateVolume, volume_path) == porto.exceptions.InvalidPath
os.mkdir(volume_path)
w = c.CreateVolume(volume_path, layers=[layer_name])
assert w.path == volume_path
assert c.FindVolume(volume_path).path == volume_path
assert len(w.GetLayers()) == 1
assert w.GetLayers()[0].name == layer_name
f = open(w.path + "/file", 'r+')
assert f.read() == "test"
w.Unlink()

w = c.CreateVolume(volume_path, layers=[layer_name], space_limit=str(volume_size))
assert w.path == volume_path
assert c.FindVolume(volume_path).path == volume_path
assert len(w.GetLayers()) == 1
assert w.GetLayers()[0].name == layer_name

f = open(w.path + "/file", 'r+')
assert f.read() == "test"
assert int(w.GetProperty("space_used")) <= volume_size_eps
assert int(w.GetProperty("space_available")) > volume_size - volume_size_eps

f.write("x" * (volume_size - volume_size_eps * 2))

time.sleep(2)

assert int(w.GetProperty("space_used")) >= volume_size - volume_size_eps * 2
assert int(w.GetProperty("space_available")) < volume_size_eps * 2

assert Catch(f.write, "x" * volume_size_eps * 2) == IOError
time.sleep(2)

assert int(w.GetProperty("space_used")) >= volume_size - volume_size_eps
assert int(w.GetProperty("space_available")) < volume_size_eps

AsRoot()
ConfigurePortod('test-api', '')
AsAlice()

a = c.Create(container_name)
w.Link(a)
assert len(w.GetContainers()) == 2
assert len(w.ListVolumeLinks()) == 2
w.Unlink()
assert len(w.GetContainers()) == 1
assert len(w.ListVolumeLinks()) == 1
assert w.GetContainers()[0].name == container_name
assert Catch(l.Remove) == porto.exceptions.Busy

v.Unlink()
assert Catch(c.FindVolume, v.path) == porto.exceptions.VolumeNotFound

c.Destroy(a)
assert Catch(c.FindVolume, w.path) == porto.exceptions.VolumeNotFound

v = c.CreateVolume()
c.GetVolume(v.path)
c.DestroyVolume(v.path)

v = c.NewVolume({})
c.GetVolume(v['path'])
c.GetVolumes([v['path']])
c.DestroyVolume(v['path'])

l.Remove()
os.rmdir(volume_path)


# STORAGE

v = c.CreateVolume(storage=storage_name, private_value=volume_private)
assert v.storage.name == storage_name
assert v.private == volume_private
assert v.private_value == volume_private
assert v["private"] == volume_private
assert v.GetProperty("private") == volume_private
assert c.FindStorage(storage_name).name == storage_name
v.Destroy()

v = c.CreateVolume(storage=storage_name)
assert v.private_value == volume_private
v.Destroy()

st = c.FindStorage(storage_name)
assert st.private_value == volume_private
st.Export(storage_tarball_path)
st.Remove()
assert Catch(c.FindStorage, storage_name) == porto.exceptions.VolumeNotFound
c.ImportStorage(storage_name, storage_tarball_path, private_value=volume_private)
st = c.FindStorage(storage_name)
assert st.private_value == volume_private
st.Remove()
os.unlink(storage_tarball_path)

# META STORAGE

ms = c.CreateMetaStorage(meta_storage_name, space_limit=2**20)

ml = c.ImportLayer(layer_in_meta, tarball_path)
assert c.FindLayer(layer_in_meta).name == layer_in_meta
assert ms.FindLayer("layer").name == layer_in_meta
assert len(ms.ListLayers()) == 1

assert Catch(ms.Remove) == porto.exceptions.Busy

ms.Update()
assert ms.space_limit == 2**20
assert 0 < ms.space_available < 2**20
assert 0 < ms.space_used < 2**20
assert ms.space_used + ms.space_available == ms.space_limit

ms.Resize(space_limit=2**30)
assert ms.space_limit == 2**30

v = c.CreateVolume(storage=storage_in_meta, private=volume_private, layers=[ml])
st = ms.FindStorage("storage")
assert st.name == storage_in_meta
assert c.FindStorage(storage_in_meta).name == storage_in_meta
assert len(ms.ListStorages()) == 1
v.Destroy()

ml.Remove()

st.Export(storage_tarball_path)
st.Remove()
assert len(ms.ListStorages()) == 0
st.Import(storage_tarball_path)
assert len(ms.ListStorages()) == 1
st.Remove()

ms.Remove()


# WEAK CONTAINERS

a = c.CreateWeakContainer(container_name)
a.SetProperty("command", "sleep 60")
a.Start()
c.disconnect()
c.connect()
if Catch(c.Find, container_name) != porto.exceptions.ContainerDoesNotExist:
    Catch(c.Wait, [container_name], 1000)
    assert Catch(c.Destroy, container_name) == porto.exceptions.ContainerDoesNotExist

Catch(c.Destroy, container_name)


# PID and RECONNECT

c2 = porto.Connection(auto_reconnect=False)
c2.connect()

pid = os.fork()
if pid:
    _, status = os.waitpid(pid, 0)
    assert status == 0
else:
    c.Version()
    assert Catch(c2.Version) == porto.exceptions.SocketError
    sys.exit(0)

c2.disconnect()

c.disconnect()


# TRY CONNECT

c = porto.Connection(socket_path='/run/portod.socket.not.found', timeout=1)
start = time.time()
try:
    c.TryConnect()
except porto.exceptions.SocketError:
    pass
else:
    assert False

assert c.connected() == False
assert c.nr_connects() == 1
assert time.time() - start < 0.1


# RETRY CONNECT

c = porto.Connection(socket_path='/run/portod.socket.not.found', timeout=1)
start = time.time()
try:
    c.Connect()
except porto.exceptions.SocketTimeout:
    pass
else:
    assert False

assert c.connected() == False
assert c.nr_connects() == 2
assert time.time() - start > 0.9

# check new error on portod upgrade only in python3
if sys.version_info.major != 3:
    sys.exit(0)

# PYTHON 3 ONLY BELOW

AsRoot()

ConfigurePortod('test-api', """
daemon {
    portod_stop_timeout: 1,
    portod_shutdown_timeout: 2,
}""")

def Reload():
    time.sleep(1)
    subprocess.check_call([portod, 'reload'])

try:
    os.remove('layer.tar.gz')
except:
     pass

c = porto.Connection()
v = c.CreateVolume(layers=['ubuntu-xenial'], backend='native')
subprocess.call(['timeout', '5', 'dd', 'if=/dev/urandom', 'of=' + str(v) + '/foo', 'bs=1M', 'count=2048'])

portodReload = Thread(target=Reload)
portodReload.start()

p = subprocess.run([portoctl, 'layer', '-v', '-E', str(v), 'layer.tar.gz'], stdout = subprocess.PIPE, stderr=subprocess.PIPE)
assert p.returncode == 0

portodReload.join()

subprocess.call(['vmtouch', '-e', 'layer.tar.gz'])
p = subprocess.run([portoctl, '--disk-timeout', '1', '-t', '1', 'layer', '-I', 'ubuntu-api-test', 'layer.tar.gz'], stdout = subprocess.PIPE, stderr=subprocess.PIPE)
assert p.returncode != 0
assert str(p.stderr).find('Resource temporarily unavailable') >= 0
time.sleep(5)
assert str(os.listdir('/place/porto_layers')).find('ubuntu-api-test') == -1

# test async RemoveLayer

c.ImportLayer('test-api-layer-1', os.getcwd() + '/layer.tar.gz')
c.ImportLayer('test-api-layer-2', os.getcwd() + '/layer.tar.gz')

start = time.time()
c.RemoveLayer('test-api-layer-1')
remove_duration = time.time() - start

start = time.time()
c.RemoveLayer('test-api-layer-2', asynchronous=True)

async_remove_duration = time.time() - start
assert str(os.listdir('/place/porto_layers')).find('test-api-layer') != -1
assert remove_duration > async_remove_duration

# 5 ms - AsyncRemoverWatchdog interval
time.sleep(10)
assert str(os.listdir('/place/porto_layers')).find('test-api-layer') == -1

ConfigurePortod('test-api', '')

try:
    os.unlink(broken_tarball_path)
    os.unlink(broken_tarball_file1)
    os.unlink(broken_tarball_file2)
except:
    pass
