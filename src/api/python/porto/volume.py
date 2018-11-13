from . import exceptions
from .container import Container

class Layer(object):
    def __init__(self, conn, name, place=None, pb=None):
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
            pb = self.conn._ListStorages(self.place, self.name + "/").meta_storages[0]
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

    def GetLabel(self, label):
        for kv in self.conn.GetVolume(self.path)['labels'].get('map', []):
            if kv['key'] == label:
                return kv['val']
        raise exceptions.LabelNotFound("Label {} is not set at Volume {}".format(label, self))

    def SetLabel(self, label, value, prev_value=None):
        return self.conn.SetVolumeLabel(self.path, label, value, prev_value)

    def Export(self, tarball, compress=None, timeout=None):
        self.conn.ExportLayer(self.path, place=self.place, tarball=tarball, compress=compress, timeout=timeout)

    def Destroy(self, strict=None, timeout=None):
        self.conn.UnlinkVolume(self.path, '***', strict=strict, timeout=timeout)
