#!/usr/bin/python

import common
import signal
from random import randint
from common import *
import properties
import functools

VERBOSE=True

WaitCache = []

def set_active(is_active):
    properties.ACTIVE = is_active

def Create(conn,dest):
    if VERBOSE:
        print "Creating container: " + dest
    conn.Create(dest)

def Destroy(conn,dest):
    if VERBOSE:
        print "Destroying container: " + dest
    conn.Destroy(dest)

def Start(conn,dest):
    if VERBOSE:
        print "Starting container: " + dest
    conn.Start(dest)

def Stop(conn,dest):
    if VERBOSE:
        print "Stopping container: " + dest
    timeout = select_by_weight( [
        (200, None),
        (25, randint(0, 30)),
        (12, -randint(0, 2 ** 21))#,
#        (12, randint(30, 2 ** 21))
#       Uncomment for the full spectrum of sensations (fuzzer can life-lock of portod)
    ] )

    conn.Stop(dest, timeout)

def Pause(conn,dest):
    if VERBOSE:
        print "Pausing container: " + dest
    conn.Pause(dest)

def Resume(conn,dest):
    if VERBOSE:
        print "Resuming container: " + dest
    conn.Resume(dest)

def Wait(conn,dest):
    global RUN_TIME_LIMIT
    global WaitCache
    
    WaitCache += [dest]

    if randint(0, 4) > 0:
        if VERBOSE:
            print "Waiting containers: {}".format(WaitCache)

        conn.Wait(WaitCache, randint(1, RUN_TIME_LIMIT))
        WaitCache = []

def SetProperty(conn,dest):

    prop = select_by_weight(
            [
            (200, properties.Command),
            (10, properties.Isolate),
#            (20, properties.Respawn),
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
            (20, properties.OwnerUser),
            (20, properties.OwnerGroup),
            (12, properties.Hostname),
            (20, properties.VirtMode),
            (30, properties.Net),
            (30, properties.Ip),
            (20, functools.partial(properties.Root, conn))
            ]
    )()

    if VERBOSE:
        print "Setting container %s property %s = %s" %(dest, prop[0], prop[1])

    conn.SetProperty(dest, prop[0], prop[1])

def Kill(conn,dest):
    signo = randint(1, int(signal.NSIG) - 1)

    if VERBOSE:
        print "Killing the container: %s with %d" %(dest, signo)

    conn.Kill(dest, signo)
