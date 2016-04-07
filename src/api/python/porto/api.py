import os
import socket
import threading

from . import rpc_pb2
from . import exceptions

__all__ = ['Connection']


################################################################################
def _VarintEncoder():
    """Return an encoder for a basic varint value (does not include tag)."""

    local_chr = chr

    def EncodeVarint(write, value):
        bits = value & 0x7f
        value >>= 7
        while value:
            write(local_chr(0x80 | bits))
            bits = value & 0x7f
            value >>= 7
        return write(local_chr(bits))

    return EncodeVarint

_EncodeVarint = _VarintEncoder()


def _VarintDecoder(mask):
    """Return an encoder for a basic varint value (does not include tag).

    Decoded values will be bitwise-anded with the given mask before being
    returned, e.g. to limit them to 32 bits.  The returned decoder does not
    take the usual "end" parameter -- the caller is expected to do bounds checking
    after the fact (often the caller can defer such checking until later).  The
    decoder returns a (value, new_pos) pair.
    """

    local_ord = ord

    def DecodeVarint(buffer, pos):
        result = 0
        shift = 0
        while 1:
            b = local_ord(buffer[pos])
            result |= ((b & 0x7f) << shift)
            pos += 1
            if not (b & 0x80):
                result &= mask
                return (result, pos)
            shift += 7
            if shift >= 64:
                raise IOError('Too many bytes when decoding varint.')
    return DecodeVarint

_DecodeVarint = _VarintDecoder((1 << 64) - 1)
_DecodeVarint32 = _VarintDecoder((1 << 32) - 1)

################################################################################


class _RPC(object):
    def __init__(self, socket_path, timeout, socket_constructor, lock_constructor):
        self.lock = lock_constructor()
        self.socket_path = socket_path
        self.timeout = timeout
        self.socket_timeout = timeout
        self.socket_constructor = socket_constructor
        self.sock = None

    def connect(self):
        self.sock = self.socket_constructor(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.socket_timeout)
        try:
            self.sock.connect(self.socket_path)
        except socket.error as e:
            self.sock = None
            raise exceptions.SocketError("Cannot connect to {}: {}".format(self.socket_path, e))

    def disconnect(self):
        if self.sock is not None:
            self.sock.close()
        self.sock = None

    def _sendall(self, data, flags=0):
        try:
            return self.sock.sendall(data, flags)
        except socket.timeout:
            raise exceptions.SocketTimeout("Got timeout: {}".format(self.socket_timeout))
        except socket.error as e:
            self.sock = None
            raise exceptions.SocketError("Send error: {}".format(e))

    def _recv(self, count, flags=0):
        try:
            buf = []
            l = 0
            while l < count:
                piece = self.sock.recv(count - l, flags)
                if len(piece) == 0:  # this means that socket is in invalid state
                    raise socket.error(socket.errno.ECONNRESET, os.strerror(socket.errno.ECONNRESET))
                buf.append(piece)
                l += len(piece)
            return ''.join(buf)
        except socket.timeout:
            raise exceptions.SocketTimeout("Got timeout: {}".format(self.socket_timeout))
        except socket.error as e:
            self.sock = None
            raise exceptions.SocketError("Recv error: {}".format(e))

    def call(self, request, timeout):
        data = request.SerializeToString()
        hdr = bytearray()
        _EncodeVarint(hdr.append, len(data))

        with self.lock:
            if timeout != self.socket_timeout:
                if self.sock is not None:
                    self.sock.settimeout(timeout)
                self.socket_timeout = timeout

            if self.sock is None:
                self.connect()

            try:
                try:
                    self._sendall(hdr)
                except exceptions.SocketError:
                    self.connect()
                    self._sendall(hdr)

                self._sendall(data)

                msb = 1
                buf = ""
                while msb:
                    b = self._recv(1)
                    msb = ord(b) >> 7
                    buf += b

                length = _DecodeVarint32(buf, 0)
                resp = rpc_pb2.TContainerResponse()
                buf += self._recv(length[0])
            finally:
                if self.socket_timeout != self.timeout:
                    if self.sock is not None:
                        self.sock.settimeout(self.timeout)
                    self.socket_timeout = self.timeout

        resp.ParseFromString(buf[length[1]:])

        if resp.error != rpc_pb2.Success:
            raise exceptions.EError.Create(resp.error, resp.errorMsg)

        return resp

    def List(self):
        request = rpc_pb2.TContainerRequest()
        request.list.CopyFrom(rpc_pb2.TContainerListRequest())
        return self.call(request, self.timeout).list.name

    def Create(self, name):
        request = rpc_pb2.TContainerRequest()
        request.create.name = name
        self.call(request, self.timeout)

    def CreateWeak(self, name):
        request = rpc_pb2.TContainerRequest()
        request.createWeak.name = name
        self.call(request, self.timeout)

    def Destroy(self, name):
        request = rpc_pb2.TContainerRequest()
        request.destroy.name = name
        self.call(request, self.timeout)

    def Start(self, name):
        request = rpc_pb2.TContainerRequest()
        request.start.name = name
        self.call(request, self.timeout)

    def Stop(self, name):
        request = rpc_pb2.TContainerRequest()
        request.stop.name = name
        self.call(request, self.timeout)

    def Kill(self, name, sig):
        request = rpc_pb2.TContainerRequest()
        request.kill.name = name
        request.kill.sig = sig
        self.call(request, self.timeout)

    def Pause(self, name):
        request = rpc_pb2.TContainerRequest()
        request.pause.name = name
        self.call(request, self.timeout)

    def Resume(self, name):
        request = rpc_pb2.TContainerRequest()
        request.resume.name = name
        self.call(request, self.timeout)

    def GetProperty(self, name, property):
        request = rpc_pb2.TContainerRequest()
        request.getProperty.name = name
        request.getProperty.property = property
        res = self.call(request, self.timeout).getProperty.value
        if res == 'false':
            return False
        elif res == 'true':
            return True
        return res

    def SetProperty(self, name, property, value):
        if value is False:
            value = 'false'
        elif value is True:
            value = 'true'
        elif value is None:
            value = ''
        elif isinstance(value, (int, long)):
            value = str(value)

        request = rpc_pb2.TContainerRequest()
        request.setProperty.name = name
        request.setProperty.property = property
        request.setProperty.value = value
        self.call(request, self.timeout)

    def GetData(self, name, data):
        request = rpc_pb2.TContainerRequest()
        request.getData.name = name
        request.getData.data = data
        res = self.call(request, self.timeout).getData.value
        if res == 'false':
            return False
        elif res == 'true':
            return True
        return res

    def Get(self, containers, variables):
        request = rpc_pb2.TContainerRequest()
        request.get.name.extend(containers)
        request.get.variable.extend(variables)
        resp = self.call(request, self.timeout)
        if resp.error != rpc_pb2.Success:
            raise exceptions.EError.Create(resp.error, resp.errorMsg)

        res = {}
        for container in resp.get.list:
            var = {}
            for kv in container.keyval:
                if kv.HasField('error'):
                    var[kv.variable] = exceptions.EError.Create(kv.error, kv.errorMsg)
                    continue
                if kv.value == 'false':
                    var[kv.variable] = False
                elif kv.value == 'true':
                    var[kv.variable] = True
                else:
                    var[kv.variable] = kv.value

            res[container.name] = var

        return res

    def ConvertPath(self, path, source, destination):
        request = rpc_pb2.TConvertPathRequest()
        request.convertPath.path = path
        request.convertPath.source = source
        request.convertPath.destination = destination
        res = self.call(request, self.timeout).convertPath.path
        return res

    def Plist(self):
        request = rpc_pb2.TContainerRequest()
        request.propertyList.CopyFrom(rpc_pb2.TContainerPropertyListRequest())
        return [item.name for item in self.call(request, self.timeout).propertyList.list]

    def Dlist(self):
        request = rpc_pb2.TContainerRequest()
        request.dataList.CopyFrom(rpc_pb2.TContainerDataListRequest())
        return [item.name for item in self.call(request, self.timeout).dataList.list]

    def Wait(self, containers, timeout=None):
        request = rpc_pb2.TContainerRequest()
        request.wait.name.extend(containers)
        if timeout is not None and timeout >= 0:
            request.wait.timeout = timeout

        resp = self.call(request, None)
        if resp.error != rpc_pb2.Success:
            raise exceptions.EError.Create(resp.error, resp.errorMsg)
        return resp.wait.name

    def ListVolumeProperties(self):
        request = rpc_pb2.TContainerRequest()
        request.listVolumeProperties.CopyFrom(rpc_pb2.TVolumePropertyListRequest())
        return self.call(request, self.timeout).volumePropertyList.properties

    def ListVolumes(self, path=None, container=None):
        request = rpc_pb2.TContainerRequest()
        request.listVolumes.CopyFrom(rpc_pb2.TVolumeListRequest())
        if path:
            request.listVolumes.path = path
        if container:
            request.listVolumes.container = container
        return self.call(request, self.timeout).volumeList.volumes

    def CreateVolume(self, path=None, **properties):
        request = rpc_pb2.TContainerRequest()
        request.createVolume.CopyFrom(rpc_pb2.TVolumeCreateRequest())
        if path:
            request.createVolume.path = path
        for name, value in properties.iteritems():
            prop = request.createVolume.properties.add()
            prop.name, prop.value = name, value
        return self.call(request, self.timeout).volume

    def LinkVolume(self, path, container):
        request = rpc_pb2.TContainerRequest()
        request.linkVolume.path = path
        request.linkVolume.container = container
        self.call(request, self.timeout)

    def UnlinkVolume(self, path, container=None):
        request = rpc_pb2.TContainerRequest()
        request.unlinkVolume.path = path
        if container:
            request.unlinkVolume.container = container
        self.call(request, self.timeout)

    def TuneVolume(self, path, **properties):
        request = rpc_pb2.TContainerRequest()
        request.tuneVolume.CopyFrom(rpc_pb2.TVolumeTuneRequest())
        request.tuneVolume.path = path
        for name, value in properties.iteritems():
            prop = request.tuneVolume.properties.add()
            prop.name, prop.value = name, value
        self.call(request, self.timeout)

    def ImportLayer(self, layer, tarball, merge=False):
        request = rpc_pb2.TContainerRequest()
        request.importLayer.layer = layer
        request.importLayer.tarball = tarball
        request.importLayer.merge = merge
        self.call(request, self.timeout)

    def ExportLayer(self, volume, tarball):
        request = rpc_pb2.TContainerRequest()
        request.exportLayer.volume = volume
        request.exportLayer.tarball = tarball
        self.call(request, self.timeout)

    def RemoveLayer(self, layer):
        request = rpc_pb2.TContainerRequest()
        request.removeLayer.layer = layer
        self.call(request, self.timeout)

    def ListLayers(self):
        request = rpc_pb2.TContainerRequest()
        request.listLayers.CopyFrom(rpc_pb2.TLayerListRequest())
        return self.call(request, self.timeout).layers.layer


class Container(object):
    def __init__(self, rpc, name):
        self.rpc = rpc
        self.name = name

    def Start(self):
        self.rpc.Start(self.name)

    def Stop(self):
        self.rpc.Stop(self.name)

    def Kill(self, sig):
        self.rpc.Kill(self.name, sig)

    def Pause(self):
        self.rpc.Pause(self.name)

    def Resume(self):
        self.rpc.Resume(self.name)

    def Get(self, variables):
        return self.rpc.Get([self.name], variables)[self.name]

    def GetProperties(self):
        return self.Get(self.rpc.Plist())

    def GetProperty(self, property):
        # TODO make real property getters/setters
        return self.rpc.GetProperty(self.name, property)

    def SetProperty(self, property, value):
        self.rpc.SetProperty(self.name, property, value)

    def GetData(self, data):
        return self.rpc.GetData(self.name, data)

    def Wait(self, timeout=None):
        return self.rpc.Wait([self.name], timeout)

    def __str__(self):
        return 'Container `{}`'.format(self.name)

    def Destroy(self):
        self.rpc.Destroy(self.name)


class Layer(object):
    def __init__(self, rpc, name):
        self.rpc = rpc
        self.name = name

    def __str__(self):
        return 'Layer `{}`'.format(self.name)

    def Merge(self, tarball):
        self.rpc.ImportLayer(self.name, tarball, merge=True)

    def Remove(self):
        self.rpc.RemoveLayer(self.name)


class Volume(object):
    def __init__(self, rpc, path):
        self.rpc = rpc
        self.path = path

    def __str__(self):
        return 'Volume `{}`'.format(self.path)

    def GetProperties(self):
        return {p.name: p.value for p in self.rpc.ListVolumes(path=self.path)[0].properties}

    def GetProperty(self, name):
        return self.GetProperties()[name]

    def GetContainers(self):
        return [Container(self.rpc, c) for c in self.rpc.ListVolumes(path=self.path)[0].containers]

    def GetLayers(self):
        layers = self.GetProperty('layers')
        layers = layers.split(';') if layers else []
        return [Layer(self.rpc, l) for l in layers]

    def Link(self, container):
        if isinstance(container, Container):
            container = container.name
        self.rpc.LinkVolume(self.path, container)

    def Unlink(self, container=None):
        if isinstance(container, Container):
            container = container.name
        self.rpc.UnlinkVolume(self.path, container)

    def Tune(self, **properties):
        self.rpc.TuneVolume(self.path, **properties)

    def Export(self, tarball):
        self.rpc.ExportLayer(self.path, tarball)


class Connection(object):
    def __init__(self,
                 socket_path='/run/portod.socket',
                 timeout=5,
                 socket_constructor=socket.socket,
                 lock_constructor=threading.Lock):
        self.rpc = _RPC(socket_path=socket_path,
                        timeout=timeout,
                        socket_constructor=socket_constructor,
                        lock_constructor=lock_constructor)

    def connect(self):
        self.rpc.connect()

    def disconnect(self):
        self.rpc.disconnect()

    def List(self):
        return self.rpc.List()

    def ListContainers(self):
        return [Container(self.rpc, name) for name in self.rpc.List()]

    def Find(self, name):
        if name not in self.List():
            raise exceptions.ContainerDoesNotExist("no such container")
        return Container(self.rpc, name)

    def Create(self, name):
        self.rpc.Create(name)
        return Container(self.rpc, name)

    def CreateWeakContainer(self, name):
        self.rpc.CreateWeak(name)
        return Container(self.rpc, name)

    def Destroy(self, container):
        if isinstance(container, Container):
            container = container.name
        self.rpc.Destroy(container)

    def Start(self, name):
        self.rpc.Start(name)

    def Stop(self, name):
        self.rpc.Stop(name)

    def Kill(self, name, sig):
        self.rpc.Kill(name, sig)

    def Pause(self, name):
        self.rpc.Pause(name)

    def Resume(self, name):
        self.rpc.Resume(name)

    def Get(self, containers, variables):
        return self.rpc.Get(containers, variables)

    def GetProperty(self, name, property):
        return self.rpc.GetProperty(name, property)

    def SetProperty(self, name, property, value):
        self.rpc.SetProperty(name, property, value)

    def GetData(self, name, data):
        return self.rpc.GetData(name, data)

    def Plist(self):
        return self.rpc.Plist()

    def Dlist(self):
        return self.rpc.Dlist()

    def Vlist(self):
        return [prop.name for prop in self.rpc.ListVolumeProperties()]

    def Wait(self, containers, timeout=None):
        return self.rpc.Wait(containers, timeout)

    def CreateVolume(self, path=None, layers=[], **properties):
        if layers:
            layers = [l.name if isinstance(l, Layer) else l for l in layers]
            properties['layers'] = ';'.join(layers)
        return Volume(self.rpc, self.rpc.CreateVolume(path, **properties).path)

    def FindVolume(self, path):
        self.rpc.ListVolumes(path=path)
        return Volume(self.rpc, path)

    def DestroyVolume(self, volume, containers=None):
        path = volume.path if isinstance(volume, Volume) else volume
        if containers is None:
            containers = self.rpc.ListVolumes(path=path)[0].containers
        for container in containers:
            try:
                self.rpc.UnlinkVolume(path, container)
            except exceptions.VolumeNotLinked:
                pass
        try:
            self.rpc.ListVolumes(path=path)
            raise exceptions.Busy("layer `%s` is busy" % path)
        except exceptions.VolumeNotFound:
            pass

    def ListVolumes(self, container=None):
        if isinstance(container, Container):
            container = container.name
        return [Volume(self.rpc, v.path) for v in self.rpc.ListVolumes(container=container)]

    def TuneVolume(self, path, **properties):
        self.rpc.TuneVolume(path, **properties)

    def ImportLayer(self, name, tarball):
        self.rpc.ImportLayer(name, tarball)
        return Layer(self.rpc, name)

    def FindLayer(self, layer):
        if layer not in self.rpc.ListLayers():
            raise exceptions.LayerNotFound("layer `%s` not found" % layer)
        return Layer(self.rpc, layer)

    def RemoveLayer(self, name):
        self.rpc.RemoveLayer(name)

    def ListLayers(self):
        return [Layer(self.rpc, l) for l in self.rpc.ListLayers()]

    def ConvertPath(self, path, source, destination):
        return self.rpc.ConvertPath(self.rpc, path, source, destination)

# Example:
# from porto import *
# conn = Connection()
# conn.connect()
# print conn.Create('test')
# print conn.Plist()
# print conn.Dlist()
# print conn.GetProperty('test', 'command')
# print conn.GetData('/', 'state')
# print conn.List()
# print conn.Destroy('test')
#
# container = conn.Create('test2')
# print container.GetProperty('command')
# print container.GetData('state')
# print container.Get(['command', 'state'])
# conn.Destroy(container)
# conn.disconnect()
