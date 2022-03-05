import os
import time
import socket
import threading

from . import rpc_pb2
from . import exceptions


def _encode_message(msg, val, key=None):
    msg.SetInParent()
    if isinstance(val, dict):
        if key is not None:
            msg = getattr(msg, key)
            msg.SetInParent()
        for k, v in val.items():
            _encode_message(msg, v, k)
    elif isinstance(val, list):
        if key is not None:
            msg = getattr(msg, key)
        if isinstance(val[0], dict):
            for item in val:
                _encode_message(msg.add(), item)
        else:
            msg.extend(val)
    else:
        setattr(msg, key, val)


def _decode_message(msg):
    ret = dict()
    for dsc, val in msg.ListFields():
        key = dsc.name
        if dsc.type == dsc.TYPE_MESSAGE:
            if dsc.label == dsc.LABEL_REPEATED:
                ret[key] = [_decode_message(v) for v in val]
            else:
                ret[key] = _decode_message(val)
        elif dsc.label == dsc.LABEL_REPEATED:
            ret[key] = list(val)
        else:
            ret[key] = val
    return ret

class _RPC(object):
    def __init__(self, socket_path, timeout, socket_constructor,
                 lock_constructor, auto_reconnect, reconnect_interval):
        self.lock = lock_constructor()
        self.socket_path = socket_path
        self.timeout = timeout
        self.socket_constructor = socket_constructor
        self.sock = None
        self.sock_pid = None
        self.deadline = None
        self.auto_reconnect = auto_reconnect
        self.reconnect_interval = reconnect_interval
        self.nr_connects = 0
        self.connect_time = None
        self.async_wait_names = []
        self.async_wait_callback = None
        self.async_wait_timeout = None

    def _connect(self):
        if self.connect_time:
            diff = time.time() - self.connect_time
            if 0 < diff < self.reconnect_interval:
                time.sleep(self.reconnect_interval - diff)
        SOCK_CLOEXEC = 0o2000000
        self.sock = self.socket_constructor(socket.AF_UNIX, socket.SOCK_STREAM | SOCK_CLOEXEC)
        self._set_socket_timeout()
        self.nr_connects += 1
        self.connect_time = time.time()
        self.sock.connect(self.socket_path)
        self.sock_pid = os.getpid()
        self._resend_async_wait()

    def _check_connect(self):
        if self.sock is None:
            if self.auto_reconnect:
                self._connect()
            else:
                raise exceptions.SocketError("Porto socket is not connected")
        elif self.sock_pid != os.getpid():
            if self.auto_reconnect:
                self._connect()
            else:
                raise exceptions.SocketError("Porto socket connected by other pid {}".format(self.sock_pid))

    def _set_deadline(self, timeout):
        if timeout is None or timeout < 0:
            self.deadline = None
        else:
            self.deadline = time.time() + timeout

    def _check_deadline(self):
        if self.deadline is not None and self.deadline < time.time():
            self.sock = None
            raise exceptions.SocketTimeout("Porto connection timeout")

    def _set_socket_timeout(self):
        if self.deadline is None:
            self.sock.settimeout(None)
        else:
            timeout = self.deadline - time.time()
            if timeout > 0.001:
                self.sock.settimeout(timeout)
            else:
                self.sock = None
                raise exceptions.SocketTimeout("Porto connection timeout")

    def _recv_data(self, count):
        msg = bytearray()
        while len(msg) < count:
            self._set_socket_timeout()
            chunk = self.sock.recv(count - len(msg))
            if not chunk:
                raise socket.error(socket.errno.ECONNRESET, os.strerror(socket.errno.ECONNRESET))
            msg.extend(chunk)
        return msg

    def _recv_response(self):
        rsp = rpc_pb2.TContainerResponse()
        while True:
            length = shift = 0
            while True:
                b = self._recv_data(1)
                length |= (b[0] & 0x7f) << shift
                shift += 7
                if b[0] <= 0x7f:
                    break

            rsp.ParseFromString(bytes(self._recv_data(length)))

            if rsp.HasField('AsyncWait'):
                if self.async_wait_callback is not None:
                    if rsp.AsyncWait.HasField("label"):
                        self.async_wait_callback(name=rsp.AsyncWait.name, state=rsp.AsyncWait.state, when=rsp.AsyncWait.when, label=rsp.AsyncWait.label, value=rsp.AsyncWait.value)
                    else:
                        self.async_wait_callback(name=rsp.AsyncWait.name, state=rsp.AsyncWait.state, when=rsp.AsyncWait.when)
            else:
                return rsp

    def encode_request(self, request):
        req = request.SerializeToString()
        length = len(req)
        hdr = bytearray()
        while length > 0x7f:
            hdr.append(0x80 | (length & 0x7f))
            length >>= 7
        hdr.append(length)
        return hdr + req

    def call(self, request, call_timeout=0):
        req = self.encode_request(request)

        with self.lock:
            self._set_deadline(self.timeout)
            request_deadline = self.deadline

            while True:
                self.deadline = request_deadline
                self._check_deadline()

                try:
                    self._check_connect()
                    self._set_socket_timeout()
                    self.sock.sendall(req)

                    if call_timeout is None or call_timeout < 0:
                        self.deadline = None
                    elif self.deadline is not None:
                        self.deadline += call_timeout

                    response = self._recv_response()
                except socket.timeout as e:
                    self.sock = None
                    if not self.auto_reconnect:
                        raise exceptions.SocketTimeout("Porto connection timeout: {}".format(e))
                except socket.error as e:
                    self.sock = None
                    if not self.auto_reconnect:
                        raise exceptions.SocketError("Socket error: {}".format(e))
                else:
                    break

        if response.error != rpc_pb2.Success:
            raise exceptions.PortoException.Create(response.error, response.errorMsg)
        return response

    def connect(self, timeout=None):
        with self.lock:
            self._set_deadline(timeout if timeout is not None else self.timeout)
            while True:
                try:
                    self._check_deadline()
                    self._connect()
                except (socket.timeout, socket.error):
                    pass
                else:
                    break

    def try_connect(self, timeout=None):
        with self.lock:
            self._set_deadline(timeout if timeout is not None else self.timeout)
            try:
                self._connect()
            except socket.timeout as e:
                self.sock = None
                raise exceptions.SocketTimeout("Porto connection timeout: {}".format(e))
            except socket.error as e:
                self.sock = None
                raise exceptions.SocketError("Porto connection error: {}".format(e))

    def disconnect(self):
        with self.lock:
            if self.sock is not None:
                self.sock.close()
                self.sock = None

    def connected(self):
        with self.lock:
            return self.sock is not None

    def _resend_async_wait(self):
        if not self.async_wait_names:
            return

        request = rpc_pb2.TContainerRequest()
        request.AsyncWait.name.extend(self.async_wait_names)
        if self.async_wait_timeout is not None:
            request.AsyncWait.timeout_ms = int(self.async_wait_timeout * 1000)

        self.sock.sendall(self.encode_request(request))
        response = self._recv_response()
        if response.error != rpc_pb2.Success:
            raise exceptions.PortoException.Create(response.error, response.errorMsg)

    def async_wait(self, names, labels, callback, timeout):
        with self.lock:
            self.async_wait_names = names
            self.async_wait_callback = callback
            self.async_wait_timeout = timeout

        request = rpc_pb2.TContainerRequest()
        request.AsyncWait.name.extend(names)
        if timeout is not None:
            request.AsyncWait.timeout_ms = int(timeout * 1000)
        if labels is not None:
            request.AsyncWait.label.extend(labels)

        self.call(request)


class Container(object):
    def __init__(self, conn, name):
        assert isinstance(conn, Connection)
        self.conn = conn
        self.name = name

    def __str__(self):
        return self.name

    def __repr__(self):
        return 'Container `{}`'.format(self.name)

    def __div__(self, child):
        if self.name == "/":
            return Container(self.conn, child)
        return Container(self.conn, self.name + "/" + child)

    def __getitem__(self, key):
        return self.conn.GetProperty(self.name, key)

    def __setitem__(self, key, value):
        return self.conn.SetProperty(self.name, key, value)

    def Start(self):
        self.conn.Start(self.name)

    def Stop(self, timeout=None):
        self.conn.Stop(self.name, timeout)

    def Kill(self, sig):
        self.conn.Kill(self.name, sig)

    def Pause(self):
        self.conn.Pause(self.name)

    def Resume(self):
        self.conn.Resume(self.name)

    def Get(self, variables, nonblock=False, sync=False):
        return self.conn.Get([self.name], variables, nonblock, sync)[self.name]

    def Set(self, **kwargs):
        self.conn.Set(self.name, **kwargs)

    def GetProperties(self):
        return self.Get(self.conn.Plist())

    def GetProperty(self, key, sync=False):
        return self.conn.GetProperty(self.name, key, sync)

    def SetProperty(self, key, value):
        self.conn.SetProperty(self.name, key, value)

    def GetLabel(self, label):
        return self.conn.GetLabel(self.name, label)

    def SetLabel(self, label, value, prev_value=None):
        self.conn.SetLabel(self.name, label, value, prev_value)

    def IncLabel(self, label, add=1):
        return self.conn.IncLabel(self.name, label, add)

    def GetData(self, data, sync=False):
        return self.conn.GetData(self.name, data, sync)

    def SetSymlink(self, symlink, target):
        return self.conn.SetSymlink(self.name, symlink, target)

    def WaitContainer(self, timeout=None):
        return self.conn.WaitContainers([self.name], timeout=timeout)

    # legacy compat - timeout in ms
    def Wait(self, *args, **kwargs):
        return self.conn.Wait([self.name], *args, **kwargs)

    def Destroy(self):
        self.conn.Destroy(self.name)

    def ListVolumeLinks(self):
        return self.conn.ListVolumeLinks(container=self)

    def Dump(self):
        spec = self.conn.GetContainersSpecs([self.name])
        if len(spec) == 0:
            raise exceptions.ContainerDoesNotExist
        return spec[0]

    def LoadSpec(self, new_spec):
        spec = rpc_pb2.TContainerSpec()
        spec.CopyFrom(new_spec)
        spec.name = self.name
        self.conn.SetSpec(spec)

class Layer(object):
    def __init__(self, conn, name, place=None, pb=None):
        assert isinstance(conn, Connection)
        self.conn = conn
        self.name = name
        self.place = place
        self.owner_user = None
        self.owner_group = None
        self.last_usage = None
        self.private_value = None
        if pb is not None:
            self.Update(pb)

    def Update(self, pb=None):
        if pb is None:
            pb = self.conn._ListLayers(self.place, self.name).layers[0]
        self.owner_user = pb.owner_user
        self.owner_group = pb.owner_group
        self.last_usage = pb.last_usage
        self.private_value = pb.private_value

    def __str__(self):
        return self.name

    def __repr__(self):
        return 'Layer `{}` at `{}`'.format(self.name, self.place or "/place")

    def Merge(self, tarball, private_value=None, timeout=None):
        self.conn.MergeLayer(self.name, tarball, place=self.place, private_value=private_value, timeout=timeout)

    def Remove(self, timeout=None):
        self.conn.RemoveLayer(self.name, place=self.place, timeout=timeout)

    def Export(self, tarball, compress=None, timeout=None):
        self.conn.ReExportLayer(self.name, place=self.place, tarball=tarball, compress=compress, timeout=timeout)

    def GetPrivate(self):
        return self.conn.GetLayerPrivate(self.name, place=self.place)

    def SetPrivate(self, private_value):
        return self.conn.SetLayerPrivate(self.name, private_value, place=self.place)


class Storage(object):
    def __init__(self, conn, name, place, pb=None):
        assert isinstance(conn, Connection)
        self.conn = conn
        self.name = name
        self.place = place
        self.private_value = None
        self.owner_user = None
        self.owner_group = None
        self.last_usage = None
        if pb is not None:
            self.Update(pb)

    def __str__(self):
        return self.name

    def __repr__(self):
        return 'Storage `{}` at `{}`'.format(self.name, self.place or "/place")

    def Update(self, pb=None):
        if pb is None:
            pb = self.conn._ListStorages(self.place, self.name).storages[0]
        self.private_value = pb.private_value
        self.owner_user = pb.owner_user
        self.owner_group = pb.owner_group
        self.last_usage = pb.last_usage
        return self

    def Remove(self, timeout=None):
        self.conn.RemoveStorage(self.name, self.place, timeout=timeout)

    def Import(self, tarball, timeout=None):
        self.conn.ImportStorage(self.name, place=self.place, tarball=tarball, private_value=self.private_value, timeout=timeout)
        self.Update()

    def Export(self, tarball, timeout=None):
        self.conn.ExportStorage(self.name, place=self.place, tarball=tarball, timeout=timeout)

class MetaStorage(object):
    def __init__(self, conn, name, place, pb=None):
        assert isinstance(conn, Connection)
        self.conn = conn
        self.name = name
        self.place = place
        self.private_value = None
        self.owner_user = None
        self.owner_group = None
        self.last_usage = None
        self.space_limit = None
        self.inode_limit = None
        self.space_used = None
        self.inode_used = None
        self.space_available = None
        self.inode_available = None
        if pb is not None:
            self.Update(pb)

    def __str__(self):
        return self.name

    def __repr__(self):
        return 'MetaStorage `{}` at `{}`'.format(self.name, self.place or "/place")

    def Update(self, pb=None):
        if pb is None:
            pb = self.conn._ListStorages(self.place, self.name).meta_storages[0]
        self.private_value = pb.private_value
        self.owner_user = pb.owner_user
        self.owner_group = pb.owner_group
        self.last_usage = pb.last_usage
        self.space_limit = pb.space_limit
        self.inode_limit = pb.inode_limit
        self.space_used = pb.space_used
        self.inode_used = pb.inode_used
        self.space_available = pb.space_available
        self.inode_available = pb.inode_available
        return self

    def Resize(self, private_value=None, space_limit=None, inode_limit=None):
        self.conn.ResizeMetaStorage(self.name, self.place, private_value, space_limit, inode_limit)
        self.Update()

    def Remove(self):
        self.conn.RemoveMetaStorage(self.name, self.place)

    def ListLayers(self):
        return self.conn.ListLayers(place=self.place, mask=self.name + "/*")

    def ListStorages(self):
        return self.conn.ListStorages(place=self.place, mask=self.name + "/*")

    def FindLayer(self, subname):
        return self.conn.FindLayer(self.name + "/" + subname, place=self.place)

    def FindStorage(self, subname):
        return self.conn.FindStorage(self.name + "/" + subname, place=self.place)

class VolumeLink(object):
    def __init__(self, volume, container, target, read_only, required):
        self.volume = volume
        self.container = container
        self.target = target
        self.read_only = read_only
        self.required = required

    def __repr__(self):
        return 'VolumeLink `{}` `{}` `{}`'.format(self.volume.path, self.container.name, self.target)

    def Unlink(self):
        self.volume.Unlink(self.container)

class Volume(object):
    def __init__(self, conn, path, pb=None):
        assert isinstance(conn, Connection)
        self.conn = conn
        self.path = path
        if pb is not None:
            self.Update(pb)

    def Update(self, pb=None):
        if pb is None:
            pb = self.conn._ListVolumes(path=self.path)[0]

        self.containers = [Container(self.conn, c) for c in pb.containers]
        self.properties = {p.name: p.value for p in pb.properties}
        self.place = self.properties.get('place')
        self.private = self.properties.get('private')
        self.private_value = self.properties.get('private')
        self.id = self.properties.get('id')
        self.state = self.properties.get('state')
        self.backend = self.properties.get('backend')
        self.read_only = self.properties.get('read_only') == "true"
        self.owner_user = self.properties.get('owner_user')
        self.owner_group = self.properties.get('owner_group')

        if 'owner_container' in self.properties:
            self.owner_container = Container(self.conn, self.properties.get('owner_container'))
        else:
            self.owner_container = None

        layers = self.properties.get('layers', "")
        layers = layers.split(';') if layers else []
        self.layers = [Layer(self.conn, l, self.place) for l in layers]

        if self.properties.get('storage') and self.properties['storage'][0] != '/':
            self.storage = Storage(self.conn, self.properties['storage'], place=self.place)
        else:
            self.storage = None

        for name in ['space_limit', 'inode_limit', 'space_used', 'inode_used', 'space_available', 'inode_available', 'space_guarantee', 'inode_guarantee']:
            setattr(self, name, int(self.properties[name]) if name in self.properties else None)

        return self

    def __str__(self):
        return self.path

    def __repr__(self):
        return 'Volume `{}`'.format(self.path)

    def __getitem__(self, key):
        self.Update()
        return getattr(self, key)

    def __setitem__(self, key, value):
        self.Tune(**{key: value})
        self.Update()

    def GetProperties(self):
        self.Update()
        return self.properties

    def GetProperty(self, key):
        self.Update()
        return self.properties[key]

    def GetContainers(self):
        self.Update()
        return self.containers

    def ListVolumeLinks(self):
        return self.conn.ListVolumeLinks(volume=self)

    def GetLayers(self):
        self.Update()
        return self.layers

    def Link(self, container=None, target=None, read_only=False, required=False):
        if isinstance(container, Container):
            container = container.name
        self.conn.LinkVolume(self.path, container, target=target, read_only=read_only, required=required)

    def Unlink(self, container=None, strict=None, timeout=None):
        if isinstance(container, Container):
            container = container.name
        self.conn.UnlinkVolume(self.path, container, strict=strict, timeout=timeout)

    def Tune(self, **properties):
        self.conn.TuneVolume(self.path, **properties)

    def Export(self, tarball, compress=None, timeout=None):
        self.conn.ExportLayer(self.path, place=self.place, tarball=tarball, compress=compress, timeout=timeout)

    def Destroy(self, strict=None, timeout=None):
        self.conn.UnlinkVolume(self.path, '***', strict=strict, timeout=timeout)

    def Check(self):
        self.conn.CheckVolume(self.path)

class Property(object):
    def __init__(self, name, desc, read_only, dynamic):
        self.name = name
        self.desc = desc
        self.read_only = read_only
        self.dynamic = dynamic

    def __str__(self):
        return self.name

    def __repr__(self):
        return 'Property `{}` `{}`'.format(self.name, self.desc)

class Connection(object):
    def __init__(self,
                 socket_path='/run/portod.socket',
                 timeout=300,
                 disk_timeout=600,
                 socket_constructor=socket.socket,
                 lock_constructor=threading.Lock,
                 auto_reconnect=True,
                 reconnect_interval=0.5):
        self.rpc = _RPC(socket_path=socket_path,
                        timeout=timeout,
                        socket_constructor=socket_constructor,
                        lock_constructor=lock_constructor,
                        auto_reconnect=auto_reconnect,
                        reconnect_interval=reconnect_interval)
        self.disk_timeout = disk_timeout

    def connect(self, timeout=None):
        self.rpc.connect(timeout)

    def nr_connects(self):
        return self.rpc.nr_connects

    def disconnect(self):
        self.rpc.disconnect()

    def connected(self):
        return self.rpc.connected()

    def Connect(self, timeout=None):
        self.rpc.connect(timeout)

    def TryConnect(self, timeout=None):
        self.rpc.try_connect(timeout)

    def Disconnect(self):
        self.rpc.disconnect()

    def Call(self, command_name, response_name=None, **kwargs):
        req = rpc_pb2.TContainerRequest()
        cmd = getattr(req, command_name)
        cmd.SetInParent()
        _encode_message(cmd, kwargs)
        rsp = self.rpc.call(req)
        if hasattr(rsp, response_name or command_name):
            return _decode_message(getattr(rsp, response_name or command_name))
        return None

    def List(self, mask=None):
        request = rpc_pb2.TContainerRequest()
        request.list.CopyFrom(rpc_pb2.TContainerListRequest())
        if mask is not None:
            request.list.mask = mask
        return self.rpc.call(request).list.name

    def ListContainers(self, mask=None):
        return [Container(self, name) for name in self.List(mask)]

    def FindLabel(self, label, mask=None, state=None, value=None):
        request = rpc_pb2.TContainerRequest()
        request.FindLabel.label = label
        if mask is not None:
            request.FindLabel.mask = mask
        if state is not None:
            request.FindLabel.state = state
        if value is not None:
            request.FindLabel.value = value
        list = self.rpc.call(request).FindLabel.list
        return [{'name': l.name, 'state': l.state, 'label': l.label, 'value': l.value} for l in list]

    def Find(self, name):
        self.GetProperty(name, "state")
        return Container(self, name)

    def Create(self, name, weak=False):
        request = rpc_pb2.TContainerRequest()
        if weak:
            request.createWeak.name = name
        else:
            request.create.name = name
        self.rpc.call(request)
        return Container(self, name)

    def GetContainersSpecs(self, names):
        request = rpc_pb2.TContainerRequest()
        for name in names:
            filter = request.ListContainersBy.filters.add()
            filter.name = name
        resp = self.rpc.call(request)
        return resp.ListContainersBy.containers

    def SetSpec(self, spec):
        request = rpc_pb2.TContainerRequest()
        request.UpdateFromSpec.container.CopyFrom(spec)
        resp = self.rpc.call(request)

    def CreateSpec(self, container, volume=None, start=False):
        request = rpc_pb2.TContainerRequest()
        if container:
            request.CreateFromSpec.container.CopyFrom(container)
        if volume:
            request.CreateFromSpec.volume.CopyFrom(volume)
        request.CreateFromSpec.start = start
        resp = self.rpc.call(request)
        return Container(self, container.name)

    def CreateWeakContainer(self, name):
        return self.Create(name, weak=True)

    def Run(self, name, weak=True, start=True, wait=0, root_volume=None, private_value=None, **kwargs):
        ct = self.Create(name, weak=True)
        try:
            for key, value in kwargs.items():
                ct.SetProperty(key, value)
            if private_value is not None:
                ct.SetProperty('private', private_value)
            if root_volume is not None:
                root = self.CreateVolume(containers=name, **root_volume)
                ct.SetProperty('root', root.path)
            if start:
                ct.Start()
            if not weak:
                ct.SetProperty('weak', False)
            if wait != 0:
                ct.WaitContainer(wait)
        except exceptions.PortoException as e:
            try:
                ct.Destroy()
            except exceptions.ContainerDoesNotExist:
                pass
            raise e
        return ct

    def Destroy(self, container):
        if isinstance(container, Container):
            container = container.name
        request = rpc_pb2.TContainerRequest()
        request.destroy.name = container
        self.rpc.call(request)

    def Start(self, name, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.start.name = name
        self.rpc.call(request, timeout)

    def Stop(self, name, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.stop.name = name
        if timeout is not None and timeout >= 0:
            request.stop.timeout_ms = timeout * 1000
        else:
            timeout = 30
        self.rpc.call(request, timeout)

    def Kill(self, name, sig):
        request = rpc_pb2.TContainerRequest()
        request.kill.name = name
        request.kill.sig = sig
        self.rpc.call(request)

    def Pause(self, name):
        request = rpc_pb2.TContainerRequest()
        request.pause.name = name
        self.rpc.call(request)

    def Resume(self, name):
        request = rpc_pb2.TContainerRequest()
        request.resume.name = name
        self.rpc.call(request)

    def Get(self, containers, variables, nonblock=False, sync=False):
        request = rpc_pb2.TContainerRequest()
        request.get.name.extend(containers)
        request.get.variable.extend(variables)
        request.get.sync = sync
        if nonblock:
            request.get.nonblock = nonblock
        resp = self.rpc.call(request)
        res = {}
        for container in resp.get.list:
            var = {}
            for kv in container.keyval:
                if kv.HasField('error'):
                    var[kv.variable] = exceptions.PortoException.Create(kv.error, kv.errorMsg)
                    continue
                if kv.value == 'false':
                    var[kv.variable] = False
                elif kv.value == 'true':
                    var[kv.variable] = True
                else:
                    var[kv.variable] = kv.value

            res[container.name] = var
        return res

    def GetProperty(self, name, key, sync=False):
        request = rpc_pb2.TContainerRequest()
        request.getProperty.name = name
        request.getProperty.property = key
        request.getProperty.sync = sync
        res = self.rpc.call(request).getProperty.value
        if res == 'false':
            return False
        elif res == 'true':
            return True
        return res

    def SetProperty(self, name, key, value):
        if value is False:
            value = 'false'
        elif value is True:
            value = 'true'
        elif value is None:
            value = ''
        else:
            value = str(value)

        request = rpc_pb2.TContainerRequest()
        request.setProperty.name = name
        request.setProperty.property = key
        request.setProperty.value = value
        self.rpc.call(request)

    def Set(self, container, **kwargs):
        for name, value in kwargs.items():
            self.SetProperty(container, name, value)

    def GetData(self, name, data, sync=False):
        request = rpc_pb2.TContainerRequest()
        request.getData.name = name
        request.getData.data = data
        request.getData.sync = sync
        res = self.rpc.call(request).getData.value
        if res == 'false':
            return False
        elif res == 'true':
            return True
        return res

    def ContainerProperties(self):
        request = rpc_pb2.TContainerRequest()
        request.propertyList.CopyFrom(rpc_pb2.TContainerPropertyListRequest())
        res = {}
        for prop in self.rpc.call(request).propertyList.list:
            res[prop.name] = Property(prop.name, prop.desc, prop.read_only, prop.dynamic)
        return res

    def VolumeProperties(self):
        request = rpc_pb2.TContainerRequest()
        request.listVolumeProperties.CopyFrom(rpc_pb2.TVolumePropertyListRequest())
        res = {}
        for prop in self.rpc.call(request).volumePropertyList.properties:
            res[prop.name] = Property(prop.name, prop.desc, False, False)
        return res

    def Plist(self):
        request = rpc_pb2.TContainerRequest()
        request.propertyList.CopyFrom(rpc_pb2.TContainerPropertyListRequest())
        return [item.name for item in self.rpc.call(request).propertyList.list]

    # deprecated - now they properties
    def Dlist(self):
        request = rpc_pb2.TContainerRequest()
        request.dataList.CopyFrom(rpc_pb2.TContainerDataListRequest())
        return [item.name for item in self.rpc.call(request).dataList.list]

    def Vlist(self):
        request = rpc_pb2.TContainerRequest()
        request.listVolumeProperties.CopyFrom(rpc_pb2.TVolumePropertyListRequest())
        result = self.rpc.call(request).volumePropertyList.properties
        return [prop.name for prop in result]

    def WaitContainers(self, containers, timeout=None, labels=None):
        request = rpc_pb2.TContainerRequest()
        for ct in containers:
            request.wait.name.append(str(ct))
        if timeout is not None and timeout >= 0:
            request.wait.timeout_ms = int(timeout * 1000)
        else:
            timeout = None
        if labels is not None:
            request.label.extend(labels)
        resp = self.rpc.call(request, timeout)
        if resp.wait.name == "":
            raise exceptions.WaitContainerTimeout("Timeout {} exceeded".format(timeout))
        return resp.wait.name

    # legacy compat - timeout in ms
    def Wait(self, containers, timeout=None, timeout_s=None, labels=None):
        if timeout_s is not None:
            timeout = timeout_s
        elif timeout is not None and timeout >= 0:
            timeout = timeout / 1000.
        try:
            return self.WaitContainers(containers, timeout, labels=labels)
        except exceptions.WaitContainerTimeout:
            return ""

    def AsyncWait(self, containers, callback, timeout=None, labels=None):
        self.rpc.async_wait([str(ct) for ct in containers], labels, callback, timeout)

    def WaitLabels(self, containers, labels, timeout=None):
        request = rpc_pb2.TContainerRequest()
        for ct in containers:
            request.wait.name.append(str(ct))
        if timeout is not None and timeout >= 0:
            request.wait.timeout_ms = int(timeout * 1000)
        else:
            timeout = None
        request.wait.label.extend(labels)
        resp = self.rpc.call(request, timeout)
        if resp.wait.name == "":
            raise exceptions.WaitContainerTimeout("Timeout {} exceeded".format(timeout))
        return { 'when': resp.wait.when,
                 'name': resp.wait.name,
                 'state': resp.wait.state,
                 'label': resp.wait.label,
                 'value': resp.wait.value }

    def GetLabel(self, container, label):
        return self.GetProperty(container, label)

    def SetLabel(self, container, label, value, prev_value=None, state=None):
        req = rpc_pb2.TContainerRequest()
        req.SetLabel.name = str(container)
        req.SetLabel.label = label
        req.SetLabel.value = value
        if prev_value is not None:
            req.SetLabel.prev_value = prev_value
        if state is not None:
            req.SetLabel.state = state
        self.rpc.call(req)

    def IncLabel(self, container, label, add=1):
        req = rpc_pb2.TContainerRequest()
        req.IncLabel.name = str(container)
        req.IncLabel.label = label
        req.IncLabel.add = add
        return self.rpc.call(req).IncLabel.result

    def CreateVolume(self, path=None, layers=None, storage=None, private_value=None, timeout=None, **properties):
        if layers:
            layers = [l.name if isinstance(l, Layer) else l for l in layers]
            properties['layers'] = ';'.join(layers)

        if storage is not None:
            properties['storage'] = str(storage)

        if private_value is not None:
            properties['private'] = private_value

        request = rpc_pb2.TContainerRequest()
        request.createVolume.CopyFrom(rpc_pb2.TVolumeCreateRequest())
        if path:
            request.createVolume.path = path
        for name, value in properties.items():
            prop = request.createVolume.properties.add()
            prop.name, prop.value = name, value
        pb = self.rpc.call(request, timeout or self.disk_timeout).volume
        return Volume(self, pb.path, pb)

    def FindVolume(self, path):
        pb = self._ListVolumes(path=path)[0]
        return Volume(self, path, pb)

    def NewVolume(self, spec, timeout=None):
        req = rpc_pb2.TContainerRequest()
        req.NewVolume.SetInParent()
        _encode_message(req.NewVolume.volume, spec)
        rsp = self.rpc.call(req, timeout or self.disk_timeout)
        return _decode_message(rsp.NewVolume.volume)

    def GetVolume(self, path, container=None, timeout=None):
        req = rpc_pb2.TContainerRequest()
        req.GetVolume.SetInParent()
        if container is not None:
            req.GetVolume.container = str(container)
        req.GetVolume.path.append(path)
        rsp = self.rpc.call(req, timeout or self.disk_timeout)
        return _decode_message(rsp.GetVolume.volume[0])

    def GetVolumes(self, paths=None, container=None, timeout=None):
        req = rpc_pb2.TContainerRequest()
        req.GetVolume.SetInParent()
        if container is not None:
            req.GetVolume.container = str(container)
        if paths is not None:
            req.GetVolume.path.extend(paths)
        rsp = self.rpc.call(req, timeout or self.disk_timeout)
        return [_decode_message(v) for v in rsp.GetVolume.volume]

    def LinkVolume(self, path, container, target=None, read_only=False, required=False):
        request = rpc_pb2.TContainerRequest()
        if target is not None or required:
            command = request.LinkVolumeTarget
        else:
            command = request.linkVolume
        command.path = path
        command.container = container
        if target is not None:
            command.target = target
        if read_only:
            command.read_only = True
        if required:
            command.required = True
        self.rpc.call(request)

    def UnlinkVolume(self, path, container=None, target=None, strict=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        if target is not None:
            command = request.UnlinkVolumeTarget
        else:
            command = request.unlinkVolume
        command.path = path
        if container:
            command.container = container
        if target is not None:
            command.target = target
        if strict is not None:
            command.strict = strict
        self.rpc.call(request, timeout or self.disk_timeout)

    def DestroyVolume(self, volume, strict=None, timeout=None):
        self.UnlinkVolume(volume.path if isinstance(volume, Volume) else volume, '***', strict=strict, timeout=timeout)

    def _ListVolumes(self, path=None, container=None):
        if isinstance(container, Container):
            container = container.name
        request = rpc_pb2.TContainerRequest()
        request.listVolumes.CopyFrom(rpc_pb2.TVolumeListRequest())
        if path:
            request.listVolumes.path = path
        if container:
            request.listVolumes.container = container
        return self.rpc.call(request).volumeList.volumes

    def ListVolumes(self, path=None, container=None):
        return [Volume(self, v.path, v) for v in self._ListVolumes(path=path, container=container)]

    def ListVolumeLinks(self, volume=None, container=None):
        links = []
        for v in self._ListVolumes(path=volume.path if isinstance(volume, Volume) else volume, container=container):
            for l in v.links:
                links.append(VolumeLink(Volume(self, v.path, v), Container(self, l.container), l.target, l.read_only, l.required))
        return links

    def GetVolumeProperties(self, path):
        return {p.name: p.value for p in self._ListVolumes(path=path)[0].properties}

    def TuneVolume(self, path, **properties):
        request = rpc_pb2.TContainerRequest()
        request.tuneVolume.CopyFrom(rpc_pb2.TVolumeTuneRequest())
        request.tuneVolume.path = path
        for name, value in properties.items():
            prop = request.tuneVolume.properties.add()
            prop.name, prop.value = name, value
        self.rpc.call(request)

    def CheckVolume(self, path):
        request = rpc_pb2.TContainerRequest()
        request.checkVolume.path = path
        self.rpc.call(request)

    def ImportLayer(self, layer, tarball, place=None, private_value=None, mem_cgroup=None, timeout=None, verbose_error=False):
        request = rpc_pb2.TContainerRequest()
        request.importLayer.layer = layer
        request.importLayer.tarball = tarball
        request.importLayer.merge = False
        request.importLayer.verbose_error = verbose_error
        if place is not None:
            request.importLayer.place = place
        if private_value is not None:
            request.importLayer.private_value = private_value
        if mem_cgroup:
            request.importLayer.mem_cgroup = mem_cgroup

        self.rpc.call(request, timeout or self.disk_timeout)
        return Layer(self, layer, place)

    def MergeLayer(self, layer, tarball, place=None, private_value=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.importLayer.layer = layer
        request.importLayer.tarball = tarball
        request.importLayer.merge = True
        if place is not None:
            request.importLayer.place = place
        if private_value is not None:
            request.importLayer.private_value = private_value
        self.rpc.call(request, timeout or self.disk_timeout)
        return Layer(self, layer, place)

    def RemoveLayer(self, layer, place=None, timeout=None, asynchronous=False):
        request = rpc_pb2.TContainerRequest()
        request.removeLayer.layer = layer
        setattr(request.removeLayer, 'async', asynchronous)
        if place is not None:
            request.removeLayer.place = place
        self.rpc.call(request, timeout or self.disk_timeout)

    def GetLayerPrivate(self, layer, place=None):
        request = rpc_pb2.TContainerRequest()
        request.getlayerprivate.layer = layer
        if place is not None:
            request.getlayerprivate.place = place
        return self.rpc.call(request).layer_private.private_value

    def SetLayerPrivate(self, layer, private_value, place=None):
        request = rpc_pb2.TContainerRequest()
        request.setlayerprivate.layer = layer
        request.setlayerprivate.private_value = private_value

        if place is not None:
            request.setlayerprivate.place = place
        self.rpc.call(request)

    def ExportLayer(self, volume, tarball, place=None, compress=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.exportLayer.volume = volume
        request.exportLayer.tarball = tarball
        if place is not None:
            request.exportLayer.place = place
        if compress is not None:
            request.exportLayer.compress = compress
        self.rpc.call(request, timeout or self.disk_timeout)

    def ReExportLayer(self, layer, tarball, place=None, compress=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.exportLayer.volume = ""
        request.exportLayer.layer = layer
        request.exportLayer.tarball = tarball
        if place is not None:
            request.exportLayer.place = place
        if compress is not None:
            request.exportLayer.compress = compress
        self.rpc.call(request, timeout or self.disk_timeout)

    def _ListLayers(self, place=None, mask=None):
        request = rpc_pb2.TContainerRequest()
        request.listLayers.CopyFrom(rpc_pb2.TLayerListRequest())
        if place is not None:
            request.listLayers.place = place
        if mask is not None:
            request.listLayers.mask = mask
        return self.rpc.call(request).layers

    def ListLayers(self, place=None, mask=None):
        response = self._ListLayers(place, mask)
        if response.layers:
            return [Layer(self, l.name, place, l) for l in response.layers]
        return [Layer(self, l, place) for l in response.layer]

    def FindLayer(self, layer, place=None):
        response = self._ListLayers(place, layer)
        if layer not in response.layer:
            raise exceptions.LayerNotFound("layer `%s` not found" % layer)
        if response.layers and response.layers[0].name == layer:
            return Layer(self, layer, place, response.layers[0])
        return Layer(self, layer, place)

    def _ListStorages(self, place=None, mask=None):
        request = rpc_pb2.TContainerRequest()
        request.listStorage.CopyFrom(rpc_pb2.TStorageListRequest())
        if place is not None:
            request.listStorage.place = place
        if mask is not None:
            request.listStorage.mask = mask
        return self.rpc.call(request).storageList

    def ListStorages(self, place=None, mask=None):
        return [Storage(self, s.name, place, s) for s in self._ListStorages(place, mask).storages]

    # deprecated
    def ListStorage(self, place=None, mask=None):
        return [Storage(self, s.name, place, s) for s in self._ListStorages(place, mask).storages]

    def FindStorage(self, name, place=None):
        response = self._ListStorages(place, name)
        if not response.storages:
            raise exceptions.VolumeNotFound("storage `%s` not found" % name)
        return Storage(self, name, place, response.storages[0])

    def ListMetaStorages(self, place=None, mask=None):
        return [MetaStorage(self, s.name, place, s) for s in self._ListStorages(place, mask).meta_storages]

    def FindMetaStorage(self, name, place=None):
        response = self._ListStorages(place, name)
        if not response.meta_storages:
            raise exceptions.VolumeNotFound("meta storage `%s` not found" % name)
        return MetaStorage(self, name, place, response.meta_storages[0])

    def RemoveStorage(self, name, place=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.removeStorage.name = name
        if place is not None:
            request.removeStorage.place = place
        self.rpc.call(request, timeout or self.disk_timeout)

    def ImportStorage(self, name, tarball, place=None, private_value=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.importStorage.name = name
        request.importStorage.tarball = tarball
        if place is not None:
            request.importStorage.place = place
        if private_value is not None:
            request.importStorage.private_value = private_value
        self.rpc.call(request, timeout or self.disk_timeout)
        return Storage(self, name, place)

    def ExportStorage(self, name, tarball, place=None, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.exportStorage.name = name
        request.exportStorage.tarball = tarball
        if place is not None:
            request.exportStorage.place = place
        self.rpc.call(request, timeout or self.disk_timeout)

    def CreateMetaStorage(self, name, place=None, private_value=None, space_limit=None, inode_limit=None):
        request = rpc_pb2.TContainerRequest()
        request.CreateMetaStorage.name = name
        if place is not None:
            request.CreateMetaStorage.place = place
        if private_value is not None:
            request.CreateMetaStorage.private_value = private_value
        if space_limit is not None:
            request.CreateMetaStorage.space_limit = space_limit
        if inode_limit is not None:
            request.CreateMetaStorage.inode_limit = inode_limit
        self.rpc.call(request)
        return MetaStorage(self, name, place)

    def ResizeMetaStorage(self, name, place=None, private_value=None, space_limit=None, inode_limit=None):
        request = rpc_pb2.TContainerRequest()
        request.ResizeMetaStorage.name = name
        if place is not None:
            request.ResizeMetaStorage.place = place
        if private_value is not None:
            request.ResizeMetaStorage.private_value = private_value
        if space_limit is not None:
            request.ResizeMetaStorage.space_limit = space_limit
        if inode_limit is not None:
            request.ResizeMetaStorage.inode_limit = inode_limit
        self.rpc.call(request)

    def RemoveMetaStorage(self, name, place=None):
        request = rpc_pb2.TContainerRequest()
        request.RemoveMetaStorage.name = name
        if place is not None:
            request.RemoveMetaStorage.place = place
        self.rpc.call(request)

    def ConvertPath(self, path, source, destination):
        request = rpc_pb2.TContainerRequest()
        request.convertPath.path = path
        request.convertPath.source = source
        request.convertPath.destination = destination
        return self.rpc.call(request).convertPath.path

    def SetSymlink(self, name, symlink, target):
        request = rpc_pb2.TContainerRequest()
        request.SetSymlink.container = name
        request.SetSymlink.symlink = symlink
        request.SetSymlink.target = target
        self.rpc.call(request)

    def AttachProcess(self, name, pid, comm=""):
        request = rpc_pb2.TContainerRequest()
        request.attachProcess.name = name
        request.attachProcess.pid = pid
        request.attachProcess.comm = comm
        self.rpc.call(request)

    def AttachThread(self, name, pid, comm=""):
        request = rpc_pb2.TContainerRequest()
        request.AttachThread.name = name
        request.AttachThread.pid = pid
        request.AttachThread.comm = comm
        self.rpc.call(request)

    def LocateProcess(self, pid, comm=""):
        request = rpc_pb2.TContainerRequest()
        request.locateProcess.pid = pid
        request.locateProcess.comm = comm
        name = self.rpc.call(request).locateProcess.name
        return Container(self, name)

    def Version(self):
        request = rpc_pb2.TContainerRequest()
        request.version.CopyFrom(rpc_pb2.TVersionRequest())
        response = self.rpc.call(request)
        return (response.version.tag, response.version.revision)
