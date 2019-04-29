import porto
import pwd
import grp
import sys
import os

MIN_RAM_SIZE = 4    # in GB
MAX_RAM_SIZE = 1024 # in GB = 1 TB

ram_size = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') # in bytes
ram_size = ram_size / (1024.**3) # in GB

if ram_size <= MIN_RAM_SIZE or MAX_RAM_SIZE <= ram_size:
    print "\nERROR: Test is valid only for machines with RAM from {} GB to {} GB (not including)!\n".format(MIN_RAM_SIZE, MAX_RAM_SIZE)
    sys.exit(1)

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

if "memory_guarantee" not in c.Plist():
    print "SKIP memory_guarantee"
    sys.exit()

ct0 = c.Create("test")
ct1 = c.Create("test/test1")
ct2 = c.Create("test/test2")
ct3 = c.Create("test/test3")
ct4 = c.Create("test/test4")

ct0.SetProperty("memory_guarantee", "128")
assert ct0.GetProperty("memory_guarantee") == "128"
ct0.SetProperty("memory_guarantee", "131072")
assert ct0.GetProperty("memory_guarantee") == "131072"
ct0.SetProperty("memory_guarantee", "1G")
assert ct0.GetProperty("memory_guarantee") == "1073741824"
ct0.SetProperty("memory_guarantee", "0")
assert ct0.GetProperty("memory_guarantee") == "0"
ct0.SetProperty("memory_guarantee", "256000000")
assert ct0.GetProperty("memory_guarantee") == "256000000"
ct0.SetProperty("memory_guarantee", "1T")
assert ct0.GetProperty("memory_guarantee") == "1099511627776"

assert Catch(ct0.Start) == porto.exceptions.ResourceNotAvailable

ct0.SetProperty("memory_guarantee", "0")
assert ct0.GetProperty("memory_guarantee") == "0"

ct0.Start()

assert Catch(ct0.SetProperty, "memory_guarantee", "1T") == porto.exceptions.ResourceNotAvailable

ct1.SetProperty("memory_guarantee", "256M")
ct2.SetProperty("memory_guarantee", "256M")
ct3.SetProperty("memory_guarantee", "256M")
ct4.SetProperty("memory_guarantee", "256M")

ct1.Start()
ct2.Start()
ct3.Start()
ct4.Start()

Catch(ct4.SetProperty, "memory_guarantee", "1T")

assert Catch(ct4.SetProperty, "memory_guarantee", "1T") == porto.exceptions.ResourceNotAvailable
ct4.SetProperty("memory_guarantee", "2.0G")
assert ct4.GetProperty("memory_guarantee") == "2147483648"
assert Catch(ct4.SetProperty, "memory_guarantee", "1T") == porto.exceptions.ResourceNotAvailable
assert ct4.GetProperty("memory_guarantee") == "2147483648"
ct4.SetProperty("memory_guarantee", "0")
assert ct4.GetProperty("memory_guarantee") == "0"

ct0.Destroy()

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
r.Start()
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
