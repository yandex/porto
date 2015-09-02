from . import rpc_pb2


class SocketError(Exception):
    pass


class SocketTimeout(Exception):
    pass


class UnknownError(Exception):
    def __str__(self):
        return '{}: {}'.format(self.__class__.__name__, self.message)


class EError(Exception):
    EID = None
    __TYPES__ = {}

    class __metaclass__(type):
        def __new__(mcs, cls, bases, dct):
            new_class = type.__new__(mcs, cls, bases, dct)

            eid = dct.get('EID')
            if eid is not None:
                EError.__TYPES__[eid] = new_class

            return new_class

    @classmethod
    def Create(cls, eid, msg):
        e_class = cls.__TYPES__.get(eid)
        if e_class is not None:
            return e_class(msg)
        return UnknownError(msg)

    def __str__(self):
        return '{}: {}'.format(self.__class__.__name__, self.message)


class InvalidMethod(EError):
    EID = rpc_pb2.InvalidMethod


class ContainerAlreadyExists(EError):
    EID = rpc_pb2.ContainerAlreadyExists


class ContainerDoesNotExist(EError):
    EID = rpc_pb2.ContainerDoesNotExist


class InvalidProperty(EError):
    EID = rpc_pb2.InvalidProperty


class InvalidData(EError):
    EID = rpc_pb2.InvalidData


class InvalidValue(EError):
    EID = rpc_pb2.InvalidValue


class InvalidState(EError):
    EID = rpc_pb2.InvalidState


class NotSupported(EError):
    EID = rpc_pb2.NotSupported


class ResourceNotAvailable(EError):
    EID = rpc_pb2.ResourceNotAvailable


class PermissionError(EError):
    EID = rpc_pb2.Permission


class VolumeAlreadyExists(EError):
    EID = rpc_pb2.VolumeAlreadyExists


class VolumeNotFound(EError):
    EID = rpc_pb2.VolumeNotFound


class VolumeAlreadyLinked(EError):
    EID = rpc_pb2.VolumeAlreadyLinked


class VolumeNotLinked(EError):
    EID = rpc_pb2.VolumeNotLinked


class LayerAlreadyExists(EError):
    EID = rpc_pb2.LayerAlreadyExists


class LayerNotFound(EError):
    EID = rpc_pb2.LayerNotFound


class NoSpace(EError):
    EID = rpc_pb2.NoSpace


class Busy(EError):
    EID = rpc_pb2.Busy
