import porto
import pwd
import grp
import sys
import os
from test_common import *

def Catch(func, *args, **kwargs):
    try:
        func(*args, **kwargs)
    except:
        return sys.exc_info()[0]
    return None

#Note that test was developed on 8G machine
#TODO fix that

c = porto.Connection()
c.Connect()

if "memory_guarantee" not in c.Plist():
    print "SKIP memory_guarantee"
    sys.exit()

ct0 = c.Create("test")
ct1 = c.Create("test/test1")
ct2 = c.Create("test/test2")
ct3 = c.Create("test/test3")
ct4 = c.Create("test/test4")

ct0.SetProperty("memory_guarantee", "128")
ExpectEq(ct0.GetProperty("memory_guarantee"), "128")
ct0.SetProperty("memory_guarantee", "131072")
ExpectEq(ct0.GetProperty("memory_guarantee"), "131072")
ct0.SetProperty("memory_guarantee", "1G")
ExpectEq(ct0.GetProperty("memory_guarantee"), "1073741824")
ct0.SetProperty("memory_guarantee", "0")
ExpectEq(ct0.GetProperty("memory_guarantee"), "0")
ct0.SetProperty("memory_guarantee", "256000000")
ExpectEq(ct0.GetProperty("memory_guarantee"), "256000000")
ct0.SetProperty("memory_guarantee", "1T")
ExpectEq(ct0.GetProperty("memory_guarantee"), "1099511627776")

ExpectEq(Catch(ct0.Start), porto.exceptions.ResourceNotAvailable)

ct0.SetProperty("memory_guarantee", "0")
ExpectEq(ct0.GetProperty("memory_guarantee"), "0")

ct0.Start()

ExpectEq(Catch(ct0.SetProperty, "memory_guarantee", "1T"), porto.exceptions.ResourceNotAvailable)

ct1.SetProperty("memory_guarantee", "256M")
ct2.SetProperty("memory_guarantee", "256M")
ct3.SetProperty("memory_guarantee", "256M")
ct4.SetProperty("memory_guarantee", "256M")

ct1.Start()
ct2.Start()
ct3.Start()
ct4.Start()

Catch(ct4.SetProperty, "memory_guarantee", "1T")

ExpectEq(Catch(ct4.SetProperty, "memory_guarantee", "1T"), porto.exceptions.ResourceNotAvailable)
ct4.SetProperty("memory_guarantee", "3.0G")
ExpectEq(ct4.GetProperty("memory_guarantee"), "3221225472")
ExpectEq(Catch(ct4.SetProperty, "memory_guarantee", "1T"), porto.exceptions.ResourceNotAvailable)
ExpectEq(ct4.GetProperty("memory_guarantee"), "3221225472")
ct4.SetProperty("memory_guarantee", "0")
ExpectEq(ct4.GetProperty("memory_guarantee"), "0")

ct0.Destroy()

r = c.Create("test1")
r.SetProperty("memory_guarantee", "1M")
r = c.Create("test2")
r.SetProperty("memory_guarantee", "1M")

r = c.Create("test1/test1")
r.SetProperty("memory_guarantee", "1M")
r = c.Create("test1/test2")
ExpectEq(r.GetProperty("memory_guarantee"), "0")

r = c.Create("test2/test1")
ExpectEq(r.GetProperty("memory_guarantee"), "0")
r = c.Create("test2/test2")
r.SetProperty("memory_guarantee", "1M")

r = c.Create("test1/test2/test1")
r.SetProperty("memory_guarantee", "1M")
r = c.Create("test1/test2/test2")
r.SetProperty("memory_guarantee", "0.9M")

r = c.Create("test2/test1/test1")
r.SetProperty("memory_guarantee", "1.75M")
r = c.Create("test2/test1/test2")
ExpectEq(r.GetProperty("memory_guarantee"), "0")

r = c.Find("test1")
r.SetProperty("memory_guarantee", "2M")
ExpectEq(r.GetProperty("memory_guarantee"), "2097152")
r.SetProperty("memory_guarantee", "3M")
ExpectEq(r.GetProperty("memory_guarantee"), "3145728")

r = c.Find("test2")
r.Start()
r.SetProperty("memory_guarantee", "2M")
ExpectEq(r.GetProperty("memory_guarantee"), "2097152")
r.SetProperty("memory_guarantee", "2.6M")
ExpectEq(r.GetProperty("memory_guarantee"), "2726297")
ExpectEq(Catch(r.SetProperty, "memory_guarantee", "1000G"), porto.exceptions.ResourceNotAvailable)

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
