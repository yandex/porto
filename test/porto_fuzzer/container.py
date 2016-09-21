#!/usr/bin/python

import common
import signal
from random import randint
from common import *
import properties

def Create(conn,dest):
    print "Creating container: " + dest
    conn.Create(dest)

def Destroy(conn,dest):
    print "Destroying container: " + dest
    conn.Destroy(dest)

def Start(conn,dest):
    print "Starting container: " + dest
    conn.Start(dest)

def Stop(conn,dest):
    print "Stopping container: " + dest
    timeout = select_by_weight( [
        (200, None),
        (25, randint(0, 30)),
        (12, -randint(0, 2 ** 21)),
        (12, randint(30, 2 ** 21))
    ] )

    conn.Stop(dest, timeout)

def Pause(conn,dest):
    print "Pausing container: " + dest
    conn.Pause(dest)

def Resume(conn,dest):
    print "Resuming container: " + dest
    conn.Resume(dest)

def Wait(conn,dest):
    global RUN_TIME_LIMIT

    print "Waiting container: " + dest
    conn.Wait(dest, randint(0, RUN_TIME_LIMIT))


def SetProperty(conn,dest):

    prop = select_by_weight(
            [
            (200, properties.Command),
            (10, properties.Isolate),
            (20, properties.Respawn),
            (15, properties.MaxRespawns),
            (10, properties.Weak),
            (20, properties.AgingTime),
            (20, properties.Private),
            (25, properties.EnablePorto),
            (50, properties.MemoryLimit),
            (50, properties.MemoryGuarantee),
            (35, properties.AnonLimit),
            (35, properties.DirtyLimit),
            (30, properties.RechargeOnPgfault),
            (50, properties.CpuLimit),
            (50, properties.CpuGuarantee),
            (30, properties.CpuPolicy),
            (35, properties.IoLimit),
            (35, properties.IoOpsLimit),
            (25, properties.IoPolicy),
            (20, properties.User),
            (20, properties.Group),
            (12, properties.Hostname),
            (20, properties.VirtMode)
            ]
    )()

    print "Setting container %s property %s = %s" %(dest, prop[0], prop[1])

    conn.SetProperty(dest, prop[0], prop[1])

def Kill(conn,dest):
    signo = randint(1, int(signal.NSIG) - 1)
    print "Killing the container: %s with %d" %(dest, signo)
    conn.Kill(dest, signo)
