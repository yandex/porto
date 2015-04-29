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
    def __init__(self, socket_path, timeout):
        self.lock = threading.Lock()
        self.socket_path = socket_path
        self.timeout = timeout

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        try:
            self.sock.connect(self.socket_path)
        except socket.error as e:
            raise exceptions.SocketError("Can't open {} for write: {}".format(self.socket_path, e))

    def disconnect(self):
        self.sock.close()

    def _sendall(self, data, flags=0):
        try:
            return self.sock.sendall(data, flags)
        except socket.timeout:
            raise exceptions.SocketTimeout("Got timeout %d" % self.timeout)

    def _recv(self, count, flags=0):
        try:
            return self.sock.recv(count, flags)
        except socket.timeout:
            raise exceptions.SocketTimeout("Got timeout %d" % self.timeout)

    def call(self, request):
        data = request.SerializeToString()
        hdr = bytearray()
        _EncodeVarint(hdr.append, len(data))

        with self.lock:
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

        resp.ParseFromString(buf[length[1]:])

        if resp.error != rpc_pb2.Success:
            raise exceptions.EError.Create(resp.error, resp.errorMsg)

        return resp

    def List(self):
        request = rpc_pb2.TContainerRequest()
        request.list.CopyFrom(rpc_pb2.TContainerListRequest())
        return self.call(request).list.name

    def Create(self, name):
        request = rpc_pb2.TContainerRequest()
        request.create.name = name
        self.call(request)

    def Destroy(self, name):
        request = rpc_pb2.TContainerRequest()
        request.destroy.name = name
        self.call(request)

    def Start(self, name):
        request = rpc_pb2.TContainerRequest()
        request.start.name = name
        self.call(request)

    def Stop(self, name):
        request = rpc_pb2.TContainerRequest()
        request.stop.name = name
        self.call(request)

    def Kill(self, name, sig):
        request = rpc_pb2.TContainerRequest()
        request.kill.name = name
        request.kill.sig = sig
        self.call(request)

    def Pause(self, name):
        request = rpc_pb2.TContainerRequest()
        request.pause.name = name
        self.call(request)

    def Resume(self, name):
        request = rpc_pb2.TContainerRequest()
        request.resume.name = name
        self.call(request)

    def GetProperty(self, name, property):
        request = rpc_pb2.TContainerRequest()
        request.getProperty.name = name
        request.getProperty.property = property
        res = self.call(request).getProperty.value
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
        self.call(request)

    def GetData(self, name, data):
        request = rpc_pb2.TContainerRequest()
        request.getData.name = name
        request.getData.data = data
        res = self.call(request).getData.value
        if res == 'false':
            return False
        elif res == 'true':
            return True
        return res

    def Plist(self):
        request = rpc_pb2.TContainerRequest()
        request.propertyList.CopyFrom(rpc_pb2.TContainerPropertyListRequest())
        return [item.name for item in self.call(request).propertyList.list]

    def Dlist(self):
        request = rpc_pb2.TContainerRequest()
        request.dataList.CopyFrom(rpc_pb2.TContainerDataListRequest())
        return [item.name for item in self.call(request).dataList.list]


class Container(object):
    def __init__(self, rpc, name):
        self.rpc = rpc
        self.name = name

    def _rpc(self, request):
        return self.rpc.call(request)

    def Start(self):
        self.rpc.Start(self.name)

    def Stop(self):
        self.rpc.Stop(self.name)

    def Kill(self, sig):
        self.rpc.Kill(self.name, sig)

    def Pause(self):
        self.rpc.Pause(self.name)

    def Resume(self, name):
        self.rpc.Resume(self.name)

    def GetProperties(self):
        return {name: self.GetProperty(name) for name in self.rpc.Plist()}

    def GetProperty(self, property):
        # TODO make real property getters/setters
        return self.rpc.GetProperty(self.name, property)

    def SetProperty(self, property, value):
        self.rpc.SetProperty(self.name, property, value)

    def GetData(self, data):
        return self.rpc.GetData(self.name, data)

    def __str__(self):
        return 'Container `{}`'.format(self.name)


class Connection(object):
    def __init__(self, socket_path='/run/portod.socket', timeout=5):
        self.rpc = _RPC(socket_path=socket_path, timeout=timeout)

    def connect(self):
        self.rpc.connect()

    def disconnect(self):
        self.rpc.disconnect()

    def _rpc(self, request):
        return self.rpc.call(request)

    def List(self):
        return self.rpc.List()

    def Find(self, name):
        if name not in self.List():
            raise exceptions.ContainerDoesNotExist("no such container")
        return Container(self.rpc, name)

    def Create(self, name):
        self.rpc.Create(name)
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
        request = rpc_pb2.TContainerRequest()
        request.pause.name = name
        self.rpc.Pause(name)

    def Resume(self, name):
        self.rpc.Resume(name)

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


# Example:
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
# print container.GetData('/', 'state')
# conn.Destroy(container)
# conn.disconnect()
