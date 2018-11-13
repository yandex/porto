class Container(object):
    def __init__(self, conn, name):
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

    def __getitem__(self, prop):
        return self.conn.GetProperty(self.name, prop)

    def __setitem__(self, prop, value):
        return self.conn.SetProperty(self.name, prop, value)

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

    def GetProperty(self, prop, index=None, sync=False):
        return self.conn.GetProperty(self.name, prop, index=index, sync=sync)

    def SetProperty(self, prop, value, index=None):
        self.conn.SetProperty(self.name, prop, value, index=index)

    def GetInt(self, prop, index=None):
        return self.conn.GetInt(self.name, prop, index)

    def SetInt(self, prop, value, index=None):
        self.conn.SetInt(self.name, prop, value, index)

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
