from . import rpc_pb2

class EError(Exception):
    EID = None
    __TYPES__ = {}

    @classmethod
    def _Init(cls):
        for name, eid in rpc_pb2.EError.items():
            e_class = type(name, (cls,), {'EID': eid})
            cls.__TYPES__[eid] = e_class
            globals()[name] = e_class

    @classmethod
    def Create(cls, eid, msg):
        e_class = cls.__TYPES__.get(eid)
        if e_class is not None:
            return e_class(msg)
        return UnknownError(msg)

    def __str__(self):
        return '{}: {}'.format(self.__class__.__name__, self.message)


class SocketError(EError):
    pass


class SocketTimeout(EError):
    pass

EError._Init()

PermissionError = Permission
UnknownError = Unknown
