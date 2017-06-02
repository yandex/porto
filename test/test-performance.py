#!/usr/bin/python

import subprocess
import resource
import porto
import time
import multiprocessing
from test_common import *

NAME = os.path.basename(__file__)

def CT_NAME(suffix):
    global NAME
    return NAME + "-" + suffix

def extract_stats(values):
    ExpectNe(len(values), 0)

    tmin = values[0]; tmax = values[0]; tavg = values[0]

    for t in values[1:]:
        tmin = min(tmin, t)
        tmax = max(tmax, t)
        tavg += t

    tavg /= len(values)

    return (tmin, tmax, tavg)

def measure_time(fn):
    def _measure(*args, **kwargs):
       ts = time.time()
       ret = fn(*args, **kwargs)
       runtime = time.time() - ts
       return (runtime, ret)
    return _measure

#==================== Executors ============================

class StressExecutor(object):
    def __init__(self, *args, **kwargs):
        self.Name = kwargs.get("name")
        self.ExecTime = kwargs.get("exec_time")
        self.ToSpawn = kwargs.get("to_spawn")
        self.Seed = kwargs.get("conn_num") * self.ToSpawn

    @staticmethod
    def Params(*args, **kwargs):
        return "exec_time: {}, to_spawn: {}"\
               .format(kwargs.get("exec_time"), kwargs.get("to_spawn"))

    @staticmethod
    def Sync(pipes):
        for p in pipes:
            ExpectEq(p.recv(), "ready")

        for p in pipes:
            p.send("go")

        for p in pipes:
            ExpectEq(p.recv(), "spawn ready")

        for p in pipes:
            p.send("destroy")

    def Execute(self, pipe):
        AsAlice()
        to_spawn = self.ToSpawn

        pipe.send("ready")
        ExpectEq(pipe.recv(), "go")

        self.Conn = porto.Connection()

        spawn_times = []
        self.Cts = []

        for i in range(0, to_spawn):
            (t, ct) = self.Setup("{}-{}"\
                                 .format(self.Name, str(i)))
            spawn_times += [t]
            self.Cts += [ct]

        pipe.send("spawn ready")
        ExpectEq(pipe.recv(), "destroy")

        destroy_times = []

        for ct in self.Cts:
            (t, _) = self.Destroy(ct)
            destroy_times += [t]

        AsRoot()
        pipe.send((spawn_times, destroy_times))
        pipe.close()

    @staticmethod
    def PostProcess(values, *args, **kwargs):
        spawn = [];  destroy = [];

        for (s, d) in values:
            spawn += s; destroy += d

        (tmin, tmax, tavg) = extract_stats(spawn)
        spawn_ret = (kwargs.get("conn_num"), kwargs.get("to_spawn"), tavg, tmin,
                     sorted(spawn)[int(len(spawn) * 0.5)],
                     sorted(spawn)[int(len(spawn) * 0.9)],
                     sorted(spawn)[int(len(spawn) * 0.99)], tmax)

        (tmin, tmax, tavg) = extract_stats(destroy)
        destroy_ret = (kwargs.get("conn_num"), kwargs.get("to_spawn"), tavg, tmin,
                       sorted(destroy)[int(len(spawn) * 0.5)],
                       sorted(destroy)[int(len(spawn) * 0.9)],
                       sorted(destroy)[int(len(spawn) * 0.99)], tmax)

        return (spawn_ret, destroy_ret)



#================= Backends =======================

class Simple(object):
    @staticmethod
    def ToString():
        return "simple container"

    @measure_time
    def Setup(self, name):
        ct = self.Conn.CreateWeakContainer(name)
        ct.SetProperty("memory_limit", "1G")
        ct.SetProperty("cpu_limit", "2c")
        ct.SetProperty("command", "bash -c \'/bin/sleep {}\'"\
                       .format(self.ExecTime))
        ct.Start()
        return ct

    @measure_time
    def Get(self, ct):
        return ct.Get(self.Conn.Dlist())

    @measure_time
    def _Wait(self, ct):
        ExpectEq(ct.Wait(timeout=self.ExecTime * 1000 * 2), ct.name)

    @measure_time
    def Destroy(self, ct):
        ct.Destroy()

    def Wait(self, ct):
        (get_time, _) = self.Get(ct)
        (wait_time, _) = self._Wait(ct)
        return (get_time + wait_time, None)

class Chroot(Simple):
    @measure_time
    def Setup(self, name):
        ct = self.Conn.CreateWeakContainer(name)
        ct.SetProperty("memory_limit", "1G")
        ct.SetProperty("cpu_limit", "2c")
        ct.SetProperty("command", "bash -c \'/bin/sleep {} && sync\'"\
                       .format(self.ExecTime))

        v = self.Conn.CreateVolume(None, layers=["ubuntu-precise"])
        v.Link(ct.name)
        v.Unlink("/")

        ct.SetProperty("root", v.path)
        ct.Start()
        return ct

class NetChroot(Simple):
    @measure_time
    def Setup(self, name):
        ct = self.Conn.CreateWeakContainer(name)
        ct.SetProperty("memory_limit", "1G")
        ct.SetProperty("cpu_limit", "2c")
        ct.SetProperty("command", "bash -c \'/bin/sleep {} && sync\'"\
                       .format(self.ExecTime))

        ct.SetProperty("ip", "ip ::{:x}".format(self.Seed))
        self.Seed += 1
        ct.SetProperty("net", "L3 door")

        v = self.Conn.CreateVolume(None, layers=["ubuntu-precise"])
        v.Link(ct.name)
        v.Unlink("/")

        ct.SetProperty("root", v.path)
        ct.Start()
        return ct

#================= Runners ======================

class SimpleStressExecutor(Simple, StressExecutor):
    @staticmethod
    def ToString():
        return "simple stress executor"

class ChrootStressExecutor(Chroot, StressExecutor):
    @staticmethod
    def ToString():
        return "chroot stress executor"

class NetChrootStressExecutor(NetChroot, StressExecutor):
    @staticmethod
    def ToString():
        return "net + chroot stress executor"

#================== Loop =======================================

def Check(conn_num, ctor, *args, **kwargs):
    kwargs["conn_num"] = conn_num
    procs = []

    for i in range(0, conn_num):
        kwargs["name"] = CT_NAME(str(i))
        e = ctor(*args, **kwargs)

        parent, child = multiprocessing.Pipe()
        p = multiprocessing.Process(target=e.Execute, args=(child,),
                                    name="worker{}".format(i))
        (p.ParentPipe, p.ChildPipe) = (parent, child)
        procs += [p]

    for p in procs:
        p.start()
        p.ChildPipe.close()

    ctor.Sync([p.ParentPipe for p in procs])

    to_retry = procs
    while len(to_retry):
        skipped = []

        for p in to_retry:
            if p.is_alive():
                skipped += [p]
            else:
                p.Ret = p.ParentPipe.recv()
                p.ParentPipe.close()
                p.join(0.01)

        to_retry = skipped
        time.sleep(0.1)

    return ctor.PostProcess([p.Ret for p in procs], *args, **kwargs)

if __name__ == '__main__':
    resource.setrlimit(resource.RLIMIT_NOFILE, (32768, 32768))

    legend = "{:>8}, {:>4}, {:>5}, {:>10}, {:>10}, {:>10}, {:>10}, {:>10}, {:>10}"\
             .format("type", "num", "spwn", "avg", "min", "q50", "q90", "q99", "max")

    for ex in [SimpleStressExecutor, ChrootStressExecutor, NetChrootStressExecutor]:
        print "\nExecutor: {}\n".format(ex.ToString())
        print "Connection scaling:\n"
        print legend

        for conn_num in [4, 16, 32, 48, 64, 96, 128]:
            (s, d) = Check(conn_num, ex, exec_time=10000, to_spawn=32)

            print "{:>8}, {:4}, {:5}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}".format("spawn", *s)
            print "{:>8}, {:4}, {:5}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}".format("destroy", *d)

        print "\nContainer regression\n"
        print legend

        for to_spawn in [64, 128, 256, 512]:
            (s, d) = Check(4, ex, exec_time=10000, to_spawn=to_spawn)

            print "{:>8}, {:4}, {:5}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}".format("spawn", *s)
            print "{:>8}, {:4}, {:5}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}".format("destroy", *d)


