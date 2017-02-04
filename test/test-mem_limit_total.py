import porto
import test_common
from test_common import *

AsAlice()
d = dict()
c = porto.Connection()

def get_val(name):
    global c

    val = int(c.GetProperty(name, "memory_limit_total"))
    return val

def print_val(name):
    global c

    state = c.GetProperty(name, "state")
    val = get_val(name)
    print "%s (%s) : %d" %(name, state, val)


def dump_val():
    global c

    for i in c.List():
        print_val(i)

def cleanup():
    global c

    for i in c.List():
        if i != "/":
            Catch(c.Destroy, i)

def verify(d):
    global c

    for i in c.List():
        val = str(get_val(i))
        assert d[i] == val

cmd = "sleep 1000"

d["/"] = "0"
c.Create("a")
d["a"] = "0"
c.Create("a/b1")
d["a/b1"] = "0"
c.Create("a/b2")
d["a/b2"] = "0"
c.Create("a/b3")
d["a/b3"] = "0"
c.Create("a/b2/c1")
d["a/b2/c1"] = "0"
c.Create("a/b2/c2")
d["a/b2/c2"] = "0"
c.Create("a/b3/c1")
d["a/b3/c1"] = "0"
c.Create("a/b3/c2")
d["a/b3/c2"] = "0"
c.Create("a/b3/c2/d1")
d["a/b3/c2/d1"] = "0"

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

c.SetProperty("a/b3/c2/d1", "memory_limit", "5M")
d["a/b3/c2/d1"] = "5242880"
c.SetProperty("a/b3/c2/d1", "command", cmd)


print "Pre-start"
verify(d)


print "Setting limit for a/b3/c2"
c.SetProperty("a/b3/c2","memory_limit", "55M")
d["a/b3/c2"] = "57671680"
d["a/b3"] = "191889408"
d["a"] = "233832448"
d["/"] = "233832448"
verify(d)

print "Starting a/b3/c2/d1"
c.Start("a/b3/c2/d1")
verify(d)

print "Stopping a/b3"
c.Stop("a/b3")
verify(d)

cleanup()
