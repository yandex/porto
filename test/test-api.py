from test_common import *

import sys
import os
import porto

AsAlice()

c = porto.Connection()
c.connect()

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
c.Destroy(a)

assert Catch(c.Find, container_name) == porto.exceptions.ContainerDoesNotExist
assert not container_name in c.List()

a = c.Run(container_name, command="sleep 5", private_value=volume_private)
assert a["command"] == "sleep 5"
assert a["private"] == volume_private
a.Destroy()


# LAYERS

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
assert int(w.GetProperty("space_used")) >= volume_size - volume_size_eps * 2
assert int(w.GetProperty("space_available")) < volume_size_eps * 2
assert Catch(f.write, "x" * volume_size_eps * 2) == IOError
assert int(w.GetProperty("space_used")) >= volume_size - volume_size_eps
assert int(w.GetProperty("space_available")) < volume_size_eps

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

assert c.nr_connects() == 2
assert time.time() - start > 0.9
