#!/usr/bin/python -u

import pwd
import grp
import psutil
import sys
import random
from random import randint
from random import random as randf

from common import *

def Command():
    cpunr = psutil.cpu_count()
    return ("command",
            select_by_weight( [
                (2, "sleep " + str(
                    select_by_weight( [
                        (5, randint(0, RUN_TIME_LIMIT)),
                        (1, randint(RUN_TIME_LIMIT + 1, sys.maxint))
                    ] )
                ) ),
                (2, "python /tmp/mem_test.py " +
                        str(
                            randint(0, 50000),
                        ) + " " +
                        str(randint(1, 1048576) * PAGE_SIZE)
                ),
                (2, "bash /tmp/cpu_test.sh " +
                        str(
                            randint(0, cpunr)
                        ) + " " +
                        str(
                            randint(0, 16384)
                        )
                )#,
#                (2, "bash /tmp/io_test.sh " +
#                        str(
#                            2 ** randint(0, 16)
#                        ) + " " +
#                        str(
#                            randint(0, 50000)
#                        )
#                )
            ] )
        )


def Isolate():
    return ("isolate", select_equal(["true", "false"]) )

def Respawn():
    return ("respawn", select_equal(["true", "false"]) )

def MaxRespawns():
    return ("max_respawns", str(randint(0,10)))

def Weak():
    return ("weak", select_equal(["true", "false"]) )

def AgingTime():
    return ("aging_time", str(
                    select_by_weight(
                        [
                        (50, randint(0, RUN_TIME_LIMIT)),
                        (15, randint(RUN_TIME_LIMIT + 1, 10 * RUN_TIME_LIMIT)),
                        (15, randint(10 * RUN_TIME_LIMIT + 1, sys.maxint)),
                        (15, -randint(0, sys.maxint))
                        ]
                    )
                )
           )

def Private():
    return ("private", get_random_str(256))

def EnablePorto():
    return ("enable_porto", select_by_weight( [(1, "true"), (9, "false")] ) )

def MemoryLimit():
    total = psutil.virtual_memory().total
    return ("memory_limit", str(
            select_by_weight( [
                (3, randint(total / 32, total / 2)),
                (3, randint(total / 2, total)),
                (1, randint(total + 1, sys.maxint)),
                (2, randint(0, total / 32))
            ] )
        )
    )

def MemoryGuarantee():
    avail = psutil.virtual_memory().available
    return ("memory_guarantee", str(
            select_by_weight( [
                (3, randint(avail / 32, avail / 2)),
                (3, randint(avail / 2, avail)),
                (1, randint(avail + 1, sys.maxint)),
                (2, randint(0, avail / 32))
            ] )
        )
    )

def AnonLimit():
    total = psutil.virtual_memory().total
    return ("anon_limit", str(
            select_by_weight( [
                (6, randint(total / 32, total) / PAGE_SIZE),
                (1, randint(0, total / 32) / PAGE_SIZE),
                (1, randint(total + 1, sys.maxint) / PAGE_SIZE)
            ] )
        )
    )

def DirtyLimit():
    total = psutil.virtual_memory().total
    return ("dirty_limit", str(
            select_by_weight( [
                (6, randint(total / 32, total) / PAGE_SIZE),
                (1, randint(0, total / 32) / PAGE_SIZE),
                (1, randint(total + 1, sys.maxint) / PAGE_SIZE)
            ] )
        )
    )

def RechargeOnPgfault():
    return ("recharge_on_pgfault", select_equal(["true", "false"]) )

def CpuLimit():
    cpunr = psutil.cpu_count()
    return ("cpu_limit",
            select_by_weight( [
                (12, select_by_weight( [
                    (1, str(randint(0, cpunr * 100))),
                    (1, str(randf()) + "c")
                ] ) ),
                (1, select_equal(["0", "0c"])),
                (2, select_by_weight( [
                    (1, str(randint(cpunr * 100 + 1, sys.maxint))),
                    (1, str(randf() * randint(1, sys.maxint)) + "c")
                ] ) )
            ] )
   )

def CpuGuarantee():
    util = psutil.cpu_percent()
    cpunr = psutil.cpu_count()
    return ("cpu_guarantee",
            select_by_weight( [
                (12, select_equal( [
                    str( randint(0, int(util * cpunr)) ),
                    str( randf() * util ) + "c"
                ] ) ),
                (1, select_equal(["0", "0c"])),
                (2, select_equal( [
                    str( randint(int(util * cpunr), sys.maxint) ),
                    str( randf() * randint(1, sys.maxint) )
                ] ) )
            ] )
    )

def CpuPolicy():
    return ("cpu_policy", select_by_weight( [
                (1, "normal"),
                (1, "rt"),
                (1, "idle")
            ] )
    )

def IoLimit():
    return ("io_limit", select_by_weight( [
                (6, str(randint(0, 4 * 1024 * 1024)) + "K" ),
                (1, str(randint(4 * 1024 * 1024 + 1, sys.maxint)) )
            ] )
    )

def IoOpsLimit():
    return ("io_ops_limit", select_by_weight( [
                (6, str(randint(0, 128 * 1024)) ),
                (1, str(randint(128 * 1024 + 1, sys.maxint)) )
            ] )
    )

def IoPolicy():
    return ("io_policy", select_equal(["normal","rt","idle"]) )

def User():
    users = pwd.getpwall()
    porto_members = grp.getgrnam("porto").gr_mem
    return ("user", select_by_weight( [
                (4, porto_members[randint(0, len(porto_members) - 1)] ),
                (1, users[randint(0, len(users) - 1)].pw_name ),
                (1, str(randint(0, 65536)) )
        ] )
    )

def Group():
    groups = grp.getgrall()
    return ("group", select_by_weight( [
                (4, groups[randint(0, len(groups) - 1)].gr_name ),
                (1, str(randint(0, 65536)) )
        ] )
    )

def Hostname():
    return ("hostname", get_random_str(32) )

def VirtMode():
    return ("virt_mode", select_by_weight([ (2, "false"), (1, "true") ]) )
