#!/usr/bin/python

import subprocess
import resource
import porto
import time
import multiprocessing
from test_common import *

EPS = 0.5
DEADLINE = 5.0
NAME = os.path.basename(__file__)
CPUNR = multiprocessing.cpu_count()

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

class Executor(object):
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


    HasChroot = False
    HasNet = False
    Type = "single"

    @staticmethod
    def ToString():
        return "simple container"

    @measure_time
    def Get(self, ct):
        return ct.Get(self.Conn.Plist())

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

    @measure_time
    def Setup(self, name):
        ct = self.Conn.CreateWeakContainer(name)

        if self.Type == "meta":
            child = self.Conn.CreateWeakContainer(name + "/hook")
        elif self.Type == "slot":
            meta = self.Conn.CreateWeakContainer(name + "/meta")
            child = self.Conn.CreateWeakContainer(name + "/meta/hook")
        elif self.Type == "single":
            child = ct
        else:
            raise AssertionError("invalid test type")

        ct.SetProperty("memory_limit", "1G")
        ct.SetProperty("cpu_limit", "2c")
        ct.SetProperty("command", "bash -c \'/bin/sleep {} && sync\'"\
                       .format(self.ExecTime))

        if self.HasNet:
            ct.SetProperty("ip", "ip ::{:x}".format(self.Seed))
            self.Seed += 1
            ct.SetProperty("net", "L3 door")

        if self.HasChroot:
            v = self.Conn.CreateVolume(None, layers=["ubuntu-precise"])
            v.Link(ct.name)
            v.Unlink("/")
            ct.SetProperty("root", v.path)

        child.Start()
        return ct

#================= Runners ======================

class SimpleExecutor(Executor):
    @staticmethod
    def ToString():
        return "simple stress executor"

class ChrootExecutor(Executor):
    @staticmethod
    def ToString():
        return "chroot stress executor"

    def __init__(self, *args, **kwargs):
        super(ChrootExecutor, self).__init__(*args, **kwargs)
        self.HasChroot = True

class NetChrootExecutor(Executor):
    @staticmethod
    def ToString():
        return "net + chroot stress executor"

    def __init__(self, *args, **kwargs):
        super(NetChrootExecutor, self).__init__(*args, **kwargs)
        self.HasChroot = True
        self.HasNet = True

class MetaExecutor(Executor):
    @staticmethod
    def ToString():
        return "meta simple stress executor"

    def __init__(self, *args, **kwargs):
        super(MetaExecutor, self).__init__(*args, **kwargs)
        self.Type = "meta"

class SlotExecutor(Executor):
    @staticmethod
    def ToString():
        return "slot simple stress executor"

    def __init__(self, *args, **kwargs):
        super(SlotExecutor, self).__init__(*args, **kwargs)
        self.Type = "slot"

class MetaChrootExecutor(Executor):
    @staticmethod
    def ToString():
        return "meta chroot stress executor"

    def __init__(self, *args, **kwargs):
        super(MetaChrootExecutor, self).__init__(*args, **kwargs)
        self.Type = "meta"
        self.HasChroot = True

class SlotChrootExecutor(Executor):
    @staticmethod
    def ToString():
        return "slot chroot stress executor"

    def __init__(self, *args, **kwargs):
        super(SlotChrootExecutor, self).__init__(*args, **kwargs)
        self.Type = "slot"
        self.HasChroot = True

class MetaNetChrootExecutor(Executor):
    @staticmethod
    def ToString():
        return "meta net chroot stress executor"

    def __init__(self, *args, **kwargs):
        super(MetaNetChrootExecutor, self).__init__(*args, **kwargs)
        self.Type = "meta"
        self.HasChroot = True
        self.HasNet = True

class SlotNetChrootExecutor(Executor):
    @staticmethod
    def ToString():
        return "slot net chroot stress executor"

    def __init__(self, *args, **kwargs):
        super(SlotNetChrootExecutor, self).__init__(*args, **kwargs)
        self.Type = "slot"
        self.HasChroot = True
        self.HasNet = True

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

def PrintFormatted(name, stats):
    print "{:>8}, {:4}, {:5}, {:10.6f}, {:10.6f}, "\
          "{:10.6f}, {:10.6f}, {:10.6f}, {:10.6f}".format(name, *stats)

if __name__ == '__main__':
    resource.setrlimit(resource.RLIMIT_NOFILE, (32768, 32768))

    legend = "{:>8}, {:>4}, {:>5}, {:>10}, {:>10}, {:>10}, {:>10}, {:>10}, {:>10}"\
             .format("type", "num", "spwn", "avg", "min", "q50", "q90", "q99", "max")

    print "Connection scaling:\n"

    ex = SimpleExecutor
    print "\nExecutor: {}".format(ex.ToString())
    print legend

    (s, d) = Check(1, ex, exec_time=300, to_spawn=32)
    PrintFormatted("spawn", s)
    PrintFormatted("destroy", d)

    ExpectLe(s[4], 1.0, "simple single container creation q50 above 1 s ")
    ExpectLe(d[4], 1.0, "simple single container destroy q50 above 1 s ")


    for (coef, ex) in [(3, ChrootExecutor), (6, MetaChrootExecutor)]:

        print "\nExecutor: {}".format(ex.ToString())
        print legend

        (s, d) = Check(1, ex, exec_time=300, to_spawn=16)
        PrintFormatted("spawn", s)
        PrintFormatted("destroy", d)

        ExpectLe(s[4], DEADLINE, "q50 create time above {} s ".format(DEADLINE))
        ExpectLe(d[4], DEADLINE, "q50 destroy time above {} s ".format(DEADLINE))

        simple_avg_create = s[2]
        simple_avg_destroy = d[2]

        for conn_num in [4, 8, 16]:
            (s, d) = Check(conn_num, ex, exec_time=300, to_spawn=16)

            PrintFormatted("spawn", s)
            PrintFormatted("destroy", d)

            coef *= conn_num

            ExpectLe(s[4], DEADLINE, "q50 create time above {} s ".format(DEADLINE))
            ExpectLe(d[4], DEADLINE, "q50 destroy time above {} s ".format(DEADLINE))
            ExpectLe(s[4], coef * simple_avg_create, "q50 create time above linear ")
            ExpectLe(d[4], coef * simple_avg_destroy, "q50 create time above linear ")

    print "\nContainer regression\n"

    for ex in [MetaNetChrootExecutor]:

        print "\nExecutor: {}".format(ex.ToString())
        print legend

        prev_avg = (0.0, 0.0)
        regression_create = True
        regression_destroy = True

        for to_spawn in [10, 20, 40, 60]:
            (s, d) = Check(1, ex, exec_time=300, to_spawn=to_spawn)

            PrintFormatted("spawn", s)
            PrintFormatted("destroy", d)

            ExpectLe(s[4], DEADLINE, "q50 create time above {} s ".format(DEADLINE))
            ExpectLe(d[4], DEADLINE, "q50 destroy time above {} s ".format(DEADLINE))

            regression_create &= s[4] > (prev_avg[0] + EPS)
            regression_destroy &= d[4] > (prev_avg[1] + EPS)

            prev_avg = (s[4], d[4])

        Expect(not regression_create)
        Expect(not regression_destroy)
