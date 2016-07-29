import porto
import pwd
import grp
import sys
import os

def Catch(func, *args, **kwargs):
    try:
        func(*args, **kwargs)
    except:
        return sys.exc_info()[0]
    return None

#Note that test was developed on 8G machine
#TODO fix that

c = porto.Connection()
c.connect()
r = c.Create("test")
r = c.Create("testa")
r = c.Create("test/test1")
r = c.Create("test/test2")
r = c.Create("test/test3")
r = c.Create("test/testa")
r = c.Find("test")
r.SetProperty("memory_guarantee", "128")
assert r.GetProperty("memory_guarantee") == "128"
r.SetProperty("memory_guarantee", "131072")
assert r.GetProperty("memory_guarantee") == "131072"
r.SetProperty("memory_guarantee", "1G")
assert r.GetProperty("memory_guarantee") == "1073741824"
r.SetProperty("memory_guarantee", "0")
assert r.GetProperty("memory_guarantee") == "0"
r.SetProperty("memory_guarantee", "256000000")
assert r.GetProperty("memory_guarantee") == "256000000"
r = c.Find("test/test1")
r.SetProperty("memory_guarantee", "384000000")
r = c.Find("test/test2")
r.SetProperty("memory_guarantee", "384000000")
r = c.Find("test/test3")
r.SetProperty("memory_guarantee", "384000000")
r = c.Find("test/testa")
assert Catch(r.SetProperty, "memory_guarantee", "1000G") == porto.exceptions.ResourceNotAvailable
r = c.Find("testa")
r.SetProperty("memory_guarantee", "3.0G")
assert r.GetProperty("memory_guarantee") == "3221225472"
assert Catch(r.SetProperty, "memory_guarantee", "1000G") == porto.exceptions.ResourceNotAvailable
assert r.GetProperty("memory_guarantee") == "3221225472"
r.SetProperty("memory_guarantee", "0")
assert r.GetProperty("memory_guarantee") == "0"
c.Destroy("test/testa")
c.Destroy("test/test3")
c.Destroy("test/test2")
c.Destroy("test/test1")
c.Destroy("testa")
c.Destroy("test")

r = c.Create("test1")
r.SetProperty("memory_guarantee", "1M")
r = c.Create("test2")
r.SetProperty("memory_guarantee", "1M")

r = c.Create("test1/test1")
r.SetProperty("memory_guarantee", "1M")
r = c.Create("test1/test2")
assert r.GetProperty("memory_guarantee") == "0"

r = c.Create("test2/test1")
assert r.GetProperty("memory_guarantee") == "0"
r = c.Create("test2/test2")
r.SetProperty("memory_guarantee", "1M")

r = c.Create("test1/test2/test1")
r.SetProperty("memory_guarantee", "1M")
r = c.Create("test1/test2/test2")
r.SetProperty("memory_guarantee", "0.9M")

r = c.Create("test2/test1/test1")
r.SetProperty("memory_guarantee", "1.75M")
r = c.Create("test2/test1/test2")
assert r.GetProperty("memory_guarantee") == "0"

r = c.Find("test1")
r.SetProperty("memory_guarantee", "2M")
assert r.GetProperty("memory_guarantee") == "2097152"
r.SetProperty("memory_guarantee", "3M")
assert r.GetProperty("memory_guarantee") == "3145728"

r = c.Find("test2")
r.SetProperty("memory_guarantee", "2M")
assert r.GetProperty("memory_guarantee") == "2097152"
r.SetProperty("memory_guarantee", "2.6M")
assert r.GetProperty("memory_guarantee") == "2726297"
assert Catch(r.SetProperty, "memory_guarantee", "1000G") == porto.exceptions.ResourceNotAvailable

c.Destroy("test1/test2/test1")
c.Destroy("test1/test2/test2")
c.Destroy("test2/test1/test1")
c.Destroy("test2/test1/test2")

c.Destroy("test1/test1")
c.Destroy("test1/test2")
c.Destroy("test2/test1")
c.Destroy("test2/test2")

c.Destroy("test1")
c.Destroy("test2")

print "porto memory guarantee overcommit prevention test OK!"
