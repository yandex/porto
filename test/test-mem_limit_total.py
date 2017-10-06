import porto
import test_common
from test_common import *

d = dict()
c = porto.Connection()

def verify(d):
    for ct in d:
        ExpectProp(c.Find(ct), "memory_limit_total", d[ct])

cmd = "sleep 1000"

d["/"] = ""
c.Create("a")
d["a"] = ""
c.Create("a/b1")
d["a/b1"] = ""
c.Create("a/b2")
d["a/b2"] = ""
c.Create("a/b3")
d["a/b3"] = ""
c.Create("a/b2/c1")
d["a/b2/c1"] = ""
c.Create("a/b2/c2")
d["a/b2/c2"] = ""
c.Create("a/b3/c1")
d["a/b3/c1"] = ""
c.Create("a/b3/c2")
d["a/b3/c2"] = ""
c.Create("a/b3/c2/d1")
d["a/b3/c2/d1"] = ""

verify(d)

c.SetProperty("a/b1", "memory_limit", "10M")
d["a/b1"] = "10485760"
c.SetProperty("a/b1", "command", cmd)

c.SetProperty("a/b2/c1", "memory_limit", "15M")
d["a/b2/c1"] = "15728640"
c.SetProperty("a/b2/c1", "command", cmd)

c.SetProperty("a/b2/c2", "memory_limit", "25M")
d["a/b2/c2"] = "26214400"
c.SetProperty("a/b2/c2", "command", cmd)

c.SetProperty("a/b2", "memory_limit", "30M")
d["a/b2"] = "31457280"

c.SetProperty("a/b3/c1", "memory_limit", "128M")
d["a/b3/c1"] = "134217728"
c.SetProperty("a/b3/c1", "command", cmd)

c.SetProperty("a/b3/c2", "command", cmd)
c.SetProperty("a/b3/c2", "memory_limit", "0")

c.SetProperty("a/b3/c2/d1", "memory_limit", "5M")
d["a/b3/c2/d1"] = "5242880"
c.SetProperty("a/b3/c2/d1", "command", cmd)

c.Start("a/b1")
c.Start("a/b2")
c.Start("a/b3/c1")
c.Start("a/b3/c2/d1")

verify(d)

print "Setting limit for a/b3/c2"
c.SetProperty("a/b3/c2","memory_limit", "55M")
d["a/b3/c2"] = "57671680"
d["a/b3"] = "191889408"
d["a"] = "233832448"
d["/"] = "233832448"
verify(d)

c.Destroy("a")
