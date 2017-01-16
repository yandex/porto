import porto
import functools
import traceback
import random
import time
import sys
import os

from test_common import *

EPS = 16 * 1048576

def a_little_less(size):
    return size - EPS

EXCESS = 5.0 / 4.0

def large_enough(size):
    return size * EXCESS

(kmaj, kmin) = get_kernel_maj_min()

OLD_KERNEL = kmaj < 4 or (kmaj == 4 and kmin < 3)
HAS_ANON_LIMIT = os.access("/sys/fs/cgroup/memory/portod/memory.anon.limit", os.F_OK)
CONTAINER_MEMORY = 2048 * 1048576
GAP = 1024 * 1048576

if not HAS_ANON_LIMIT:
    print "No anon_limit found, skipping anon limit tests!"

class Allocation:
    def __init__(self, c, name, parent = None):
        self.container = c.Create(name)

        if parent is None:
            self.root_volume = c.CreateVolume(None, layers=["ubuntu-precise"], space_limit="2G")
            root_path = self.root_volume.path
            self.container.SetProperty("root", root_path)
            self.container.SetProperty("bind", "{}/mem_limit /mem_limit ro;"\
                                               .format(os.getcwd()))
        else:
            root_path = parent.root_volume.path

        tmp_path = "/tmp_bin_{}".format(name)
        os.mkdir(root_path + tmp_path)

        self.container.SetProperty("cwd", tmp_path)
        self.container.SetProperty("ulimit", "memlock: 0 0")

        if OLD_KERNEL:
            self.container.SetProperty("user", "root")
            self.container.SetProperty("group", "root")
            self.container.SetProperty("capabilities", "IPC_LOCK;DAC_OVERRIDE")
        else:
            self.container.SetProperty("capabilities", "IPC_LOCK")
            self.container.SetProperty("capabilities_ambient", "IPC_LOCK")

    def Setup(self, limit, anon_limit, file_size, anon_size):
        self.container.SetProperty("memory_limit", limit)

        if HAS_ANON_LIMIT:
            self.container.SetProperty("anon_limit", anon_limit)

        create = ""
        access = ""

        if file_size > 0:
            create += "{} {} ".format("file", int(file_size))
        if anon_size > 0:
            create += "{} {} ".format("anon", int(anon_size))

        if file_size > 0:
            access += "{} {} ".format("access_fork", "0")
        if anon_size > 0:
            access += "{} {} ".format("access", "1" if file_size > 0 else "0")

        if file_size > 0 or anon_size > 0:
            self.container.SetProperty("command", "/mem_limit %s" %(create + access))

        return self.container

def ParentAlive(r, parent, checker):
    status = parent.GetProperty("state")
    assert status == "running" or status == "meta"
    checker(r)

def IsOk(r):
    assert r.GetProperty("exit_status") == "0"

def IsOOM(r):
    assert r.GetProperty("oom_killed") == True

def CheckSingle(a, checker, *args):
    r = a.Setup(*args)
    r.Start()
    r.Wait()
    checker(r)
    r.Stop()

def Simple(c, size):
    a = Allocation(c, "simple")

    print "Checking single container limits"

    print "Checking limits achievable..."
    CheckSingle(a, IsOk, size, size / 3, a_little_less(int(size * 0.66)),
                                   a_little_less(int(size * 0.33)))
    CheckSingle(a, IsOk, size, (size / 3) * 2 , a_little_less(int(size * 0.33)),
                                          a_little_less(int(size * 0.66)))

    print "Checking limit exceeding results in OOM..."
    CheckSingle(a, IsOOM, size, size / 2, large_enough(size / 2),
                                   a_little_less(size / 2))
    CheckSingle(a, IsOOM, size, size / 2, a_little_less(size / 2),
                                   large_enough(size / 2))

    if HAS_ANON_LIMIT:
        print "Checking anon limit exceeding results in OOM..."
        CheckSingle(a, IsOOM, size, size / 2, 0, large_enough(size / 2))

    a.container.Destroy()

def CheckLoop(allocations, checkers, settings):
    num = len(allocations)

    for i in range(0, num):
        allocations[i].Setup(*settings[i])

    for a in allocations:
        a.container.Start()

    for i in range(0, num):
        r = allocations[i].container
        r.Wait()
        checkers[i](r)

    for a in allocations:
        a.container.Stop()

def Full(c, size, gap_size):
    def CheckByMask(mask):
        settings = []
        checkers = []

        for i in range(0, len(allocations)):
            if i % 3 == 0:
                settings += [(size, size / 2, a_little_less(size / 2), a_little_less(size / 2))]
                checkers += [IsOk]
            elif i % 3 == 1:
                settings += [(size, size / 2, large_enough(size / 2), a_little_less(size / 2))]
                checkers += [IsOOM]
            else:
                settings += [(size, size / 2, a_little_less(size / 2), large_enough(size / 2))]
                checkers += [IsOOM]

        CheckLoop(allocations, checkers, settings)

    mem_free = GetMeminfo("MemFree:")

    mem_alloc = mem_free - gap_size if mem_free * 0.1 < gap_size \
                else int(mem_free * 0.9)

    N = (mem_alloc / size) + 1
    size = mem_alloc / N

    allocations = []

    for i in range(0, N):
        allocations += [ Allocation(c, "even%d" %(i)) ]

    print "Checking flat container hierarchy limits"

    print "Checking all achievable..."
    CheckByMask([0] * N)

    print "Checking all excess by file..."
    CheckByMask([1] * N)

    print "Checking all excess by anon..."
    CheckByMask([2] * N)

    print "Checking patterns..."
    CheckByMask([0] * (N / 2) + [1] * (N / 2 + N % 2))
    CheckByMask([i % 3 for i in range(0, N)])

    for i in range(0, 3):
        CheckByMask([random.randint(0, 3) for i in range(0, N)])

def Hierarchical(c, size):
    def Meta():
        a = Allocation(c, "meta_parent")
        IsOkOk = functools.partial(ParentAlive, parent=a.container, checker=IsOk)
        IsOkOOM = functools.partial(ParentAlive, parent=a.container, checker=IsOOM)

        a.Setup(size, size / 2, 0, 0)
        a1 = Allocation(c, "meta_parent/c1", a)
        a2 = Allocation(c, "meta_parent/c2", a)

        CheckLoop([a1, a2], [ IsOkOk ] * 2, [(size / 2, size / 4,
                                              a_little_less(size / 4),
                                              a_little_less(size / 4))] * 2)

        CheckLoop([a1, a2], [ IsOkOk, IsOkOOM ],
                  [(size / 2, size / 4, a_little_less(size / 4), a_little_less(size / 4)),
                  (size / 2, size / 4, large_enough(size / 2), 0)])

        if HAS_ANON_LIMIT:
            CheckLoop([a1, a2], [ IsOkOk, IsOkOOM ],
                      [(size / 2, size / 4, a_little_less(size / 4), a_little_less(size / 4)),
                      (size / 2, size / 4, 0, large_enough(size / 4))])

    def Real():
        a = Allocation(c, "real_parent")
        IsOkOk = functools.partial(ParentAlive, parent=a.container, checker=IsOk)
        IsOkOOM = functools.partial(ParentAlive, parent=a.container, checker=IsOOM)

        a.Setup(size, size / 2, size / 6, size / 6)
        command_str = a.container.GetProperty("command")
        a.container.SetProperty("command", command_str + " sleep 1000")

        a1 = Allocation(c, "real_parent/c1", a)
        a2 = Allocation(c, "real_parent/c2", a)

        CheckLoop([a1, a2], [ IsOkOk ] * 2, [(size / 3, size / 6,
                                              a_little_less(size / 6),
                                              a_little_less(size / 6))] * 2)

        CheckLoop([a1, a2], [ IsOkOk, IsOkOOM ] * 2,
                  [(size / 3, size / 6, a_little_less(size / 6), a_little_less(size / 6)),
                  (size / 3, size / 6, large_enough(size / 3), 0)])

        if HAS_ANON_LIMIT:
            CheckLoop([a1, a2], [ IsOkOk, IsOkOOM ] * 2,
                      [(size / 3, size / 6, a_little_less(size / 6), a_little_less(size / 6)),
                      (size / 3, size / 6, 0, large_enough(size / 6))])

    print "Checking hierarhical limits"
    print "Checking with meta parent..."
    Meta()
    print "Checking with workload parent..."
    Real()

def LargeSimple(c, gap_size):
    a = Allocation(c, "large_simple")
    mem_free = GetMeminfo("MemFree:")

    mem_alloc = mem_free - gap_size if mem_free * 0.1 < gap_size \
                else int(mem_free * 0.9)

    size = mem_alloc

    print "Checking large memory_limit for container"
    print "Checking limit achievable..."
    CheckSingle(a, IsOk, size, 0, 0, a_little_less(size))

    print "Checking limit excessing results in OOM..."
    CheckSingle(a, IsOOM, size, 0, 0, size + gap_size * 0.5)

    if HAS_ANON_LIMIT:
        print "Checking anon limit excessing results in OOM..."
        CheckSingle(a, IsOOM, size, size / 8 * 7, 0, size + gap_size * 0.5)

def TestBody():

    if os.getuid() == 0:
        DropPrivileges()

    c = porto.Connection(timeout=30)

    Simple(c, CONTAINER_MEMORY)
    LargeSimple(c, GAP)
    Full(c, CONTAINER_MEMORY, GAP)
    Hierarchical(c, CONTAINER_MEMORY)

ret = 0

try:
    TestBody()
except BaseException as e:
    print traceback.format_exc()
    ret = 1

SwitchRoot()
c = porto.Connection(timeout=30)

if ret > 0:
    print "Dumping containers state:\n"
    for r in c.ListContainers():
        print "name : \"{}\"".format(r.name)
        DumpObjectState(r, ["command", "memory_limit", "anon_limit", "state",
                               "exit_status", "oom_killed", "stdout",
                               "memory.max_usage_in_bytes", "memory.anon.max_usage",
                               "root", "cwd", "bind", "user", "group", "capabilities",
                               "ulimit"])


for r in c.ListContainers():
    try:
        r.Destroy()
    except:
        pass

for v in c.ListVolumes():
    try:
        v.Unlink()
    except:
        pass

sys.exit(ret)
