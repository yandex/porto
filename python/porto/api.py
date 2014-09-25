import rpc_pb2
import socket
import sys

from porto import exceptions

################################################################################
def _VarintEncoder():
  """Return an encoder for a basic varint value (does not include tag)."""

  local_chr = chr
  def EncodeVarint(write, value):
    bits = value & 0x7f
    value >>= 7
    while value:
      write(local_chr(0x80|bits))
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

class PortoAPI:
    def __init__(self, socket_path='/var/run/portod.socket', timeout=5):
        self.socket_path = socket_path
        self.timeout = timeout

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        try:
            self.sock.connect(self.socket_path)
        except socket.error, msg:
            raise exceptions.SocketError("Can't open %s for write" % self.socket_path)

    def _send(self, data, flags=0):
        try:
            return self.sock.send(data, flags)
        except socket.timeout:
            raise exceptions.SocketTimeout("Got timeout %d" % self.timeout)

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

    def _rpc(self, request):
        data = request.SerializeToString()
        hdr = bytearray()
        _EncodeVarint(hdr.append, len(data))
        self._send(hdr)
        self._sendall(data)

        buf = self._recv(4)
        length = _DecodeVarint32(buf, 0)
        resp = rpc_pb2.TContainerResponse()
        buf += self._recv(length[0])
        resp.ParseFromString(buf[length[1]:])
        return resp

    def Close(self):
        self.sock.close()

    def List(self):
        request = rpc_pb2.TContainerRequest()
        request.list.CopyFrom(rpc_pb2.TContainerListRequest())
        return self._rpc(request).list.name

    def Create(self, name):
        request = rpc_pb2.TContainerRequest()
        request.create.name = name
        return self._rpc(request)

    def Destroy(self, name):
        request = rpc_pb2.TContainerRequest()
        request.destroy.name = name
        return self._rpc(request)

    def Start(self, name):
        request = rpc_pb2.TContainerRequest()
        request.start.name = name
        return self._rpc(request)

    def Stop(self, name):
        request = rpc_pb2.TContainerRequest()
        request.stop.name = name
        return self._rpc(request)

    def Kill(self, name, sig):
        request = rpc_pb2.TContainerRequest()
        request.kill.name = name
        request.kill.sig = sig
        return self._rpc(request)

    def Pause(self, name):
        request = rpc_pb2.TContainerRequest()
        request.pause.name = name
        return self._rpc(request)

    def Resume(self, name):
        request = rpc_pb2.TContainerRequest()
        request.resume.name = name
        return self._rpc(request)

    def GetProperty(self, name, property):
        request = rpc_pb2.TContainerRequest()
        request.getProperty.name = name
        request.getProperty.property = property
        return self._rpc(request)

    def SetProperty(self, name, property, value):
        request = rpc_pb2.TContainerRequest()
        request.setProperty.name = name
        request.setProperty.property = property
        request.setProperty.value = value
        return self._rpc(request)

    def GetData(self, name, data):
        request = rpc_pb2.TContainerRequest()
        request.getData.name = name
        request.getData.data = data
        return self._rpc(request)

    def Plist(self):
        request = rpc_pb2.TContainerRequest()
        request.propertyList.CopyFrom(rpc_pb2.TContainerPropertyListRequest())
        return self._rpc(request)

    def Dlist(self):
        request = rpc_pb2.TContainerRequest()
        request.dataList.CopyFrom(rpc_pb2.TContainerDataListRequest())
        return self._rpc(request)

# Example:
# rpc = PortoAPI()
# print rpc.Create('test')
# print rpc.Plist()
# print rpc.Dlist()
# print rpc.GetProperty('test', 'command')
# print rpc.GetData('/', 'state')
# print rpc.List()
# print rpc.Destroy('test')
# rpc.Close()
