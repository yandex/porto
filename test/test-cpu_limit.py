import porto
import multiprocessing
import sys
import os
import time
import traceback

#consider some difference between rt and normal scheduler
EPS = 0.3
RT_EPS = 0.3

DURATION = 1000 #ms
CPUNR = multiprocessing.cpu_count()
THREAD_NUM = CPUNR
INTERVAL = 1000 #ms
HAS_RT_LIMIT = os.access("/sys/fs/cgroup/cpu/cpu.rt_runtime_us", os.F_OK)

print "Using EPS {} for normal, EPS {} for RT, run duration {} ms".format(EPS, RT_EPS, DURATION)

def SetCommand(r, guarantee, limit, duration=DURATION, tnum=THREAD_NUM, interval=INTERVAL):
    global DURATION
    cmd = "./cpu_limit {} {} {}c {}c {}".format(tnum, duration, guarantee, limit, interval)
    r.SetProperty("command", cmd)

def InitContainer(c, name, guarantee, limit, rt=False):
    r = c.Create(name)

    if (rt):
        r.SetProperty("cpu_policy", "rt")
        eps = RT_EPS
    else:
        r.SetProperty("cpu_policy", "normal")
        eps = EPS

    if (limit != 0.0):
        assert limit + eps < float(CPUNR)
        r.SetProperty("cpu_limit", "{}c".format(limit))

    if (guarantee != 0.0):
        assert guarantee > eps
        r.SetProperty("cpu_guarantee", "{}c".format(guarantee))

    r.SetProperty("cwd", os.getcwd())

    SetCommand(r, guarantee - eps if guarantee != 0.0 else 0.0,
                  limit + eps if limit != 0.0 else 0.0)
    return r

def WaitContainer(r):
    r.Wait()

    print "{} : {}".format(r.name, r.GetProperty("stdout"))

    assert r.GetProperty("exit_status") == "0"

    r.Destroy()


def SingleContainer(c, name, guarantee, limit, rt = False):
    r = InitContainer(c, name, guarantee, limit, rt)
    r.Start()
    WaitContainer(r)
    pass

def MultipleEven(c, prefix, n, guarantee=True, limit=True, rt = False):
    r = []
    for i in range(0, n):
        r += [InitContainer(c, "{}_{}".format(prefix, i),
                            float(CPUNR) / n if guarantee else 0.0,
                            float(CPUNR) / n if limit else 0.0,
                            rt)]

    for i in r:
        i.Start()

    for i in r:
        WaitContainer(i)

def TestBody(c):
    print "Checking normal simple one-core limit"

    limit = 1.0
    guarantee = 0.0

    SingleContainer(c, "normal_one_core", 0.0, 1.0)

    if CPUNR > 1:
        print "Checking normal 1.5 core limit"
        SingleContainer(c, "normal_one_and_half_core", 0.0, 1.5)

    if CPUNR > 2:
        print "Checking normal {} - 1 core limit".format(CPUNR)
        SingleContainer(c, "normal_minus_one_core", 0.0, float(CPUNR) - 1.0)


    if HAS_RT_LIMIT:
        print "Realtime cpu limit present"
        print "Checking rt simple one-core limit"
        SingleContainer(c, "rt_one_core", 0.0, 1.0, rt=True)

        if CPUNR > 1:
            print "Checking rt 1.5 core limit"
            SingleContainer(c, "rt_one_and_half_core", 0.0, 1.5, rt=True)

        if CPUNR > 2:
            print "Checking rt {} - 1 core limit".format(CPUNR)
            SingleContainer(c, "rt_minus_one_core", 0.0, float(CPUNR) - 1.0, rt=True)

    if CPUNR > 1:
        print "Checking normal even guarantee with 2 containers"
        MultipleEven(c, "normal_half", 2, guarantee = True, limit=False, rt = False)

        print "Checking normal even limit with 2 containers"
        MultipleEven(c, "normal_half", 2, guarantee = False, limit=True, rt = False)

        if HAS_RT_LIMIT:
            print "Checking rt even limits with 2 containers"
            MultipleEven(c, "rt_half", 2, guarantee = False, limit=True, rt = True)

    if CPUNR > 2:
        print "Checking normal even guarantee with 3 containers"
        MultipleEven(c, "normal_third", 3, guarantee = True, limit=False, rt = False)

        print "Checking normal even limit with 3 containers"
        MultipleEven(c, "normal_third", 3, guarantee = False, limit=True, rt = False)

        if HAS_RT_LIMIT:
            print "Checking rt even limits with 3 containers"
            MultipleEven(c, "rt_third", 3, guarantee = False, limit=True, rt = True)

    if CPUNR > 3:
        print "Checking normal even guarantee with 4 containers"
        MultipleEven(c, "normal_fourth", 4, guarantee = True, limit=False, rt = False)

        print "Checking normal even limit with 4 containers"
        MultipleEven(c, "normal_fourth", 4, guarantee = False, limit=True, rt = False)

        if HAS_RT_LIMIT:
            print "Checking rt even limits with 4 containers"
            MultipleEven(c, "rt_fourth", 4, guarantee = False, limit=True, rt = True)

    print "Checking normal 3x0.3c limit"
    r = []
    for i in range(0, 3):
        r += [InitContainer(c, "one_third_{}".format(i),
                            0.0, 0.33)]
    for i in r:
        i.Start()

    for i in r:
        WaitContainer(i)

    if CPUNR > 3 and HAS_RT_LIMIT:
        print "Checking normal 3x1c guarantee and rt 1c limit"
        r = []
        for i in range(0, 3):
            r += [InitContainer(c, "one_third_{}".format(i),
                                1.0, 0.0)]

        r += [InitContainer(c, "rt_guy", 1.0, 1.0, rt=True)]
        for i in r:
            i.Start()

        for i in r:
            WaitContainer(i)

    if CPUNR > 7 and HAS_RT_LIMIT:
        print "Checking normal 3x1.5c guarantee/limit and rt 2x1c limit"
        r = []
        for i in range(0, 3):
            r += [InitContainer(c, "one_third_{}".format(i),
                                1.5, 1.5)]

        for i in range(0, 2):
            r += [InitContainer(c, "rt_guy_{}".format(i),
                                1.0, 1.0)]

        for i in r:
            i.Start()

        for i in r:
            WaitContainer(i)

c = porto.Connection(timeout=30)

try:
    TestBody(c)

except BaseException as e:
    print traceback.format_exc()
    for r in c.ListContainers():
        r.Destroy()

    sys.exit(1)
