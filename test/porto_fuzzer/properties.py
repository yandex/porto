#!/usr/bin/python -u

from common import *
import targets

import pwd
import grp
import sys

ACTIVE=False
ACTIVE_IO=False

cpunr = 32
ramsize = 32 << 30

def Command():
    return ("command",
            select_by_weight( [
                (2, "sleep " + str(
                    select_by_weight( [
                        (5, randint(0, RUN_TIME_LIMIT)),
                        (1, randint(RUN_TIME_LIMIT + 1, sys.maxint))
                    ] )
                )),
                (2, "echo {}".format(get_random_str(100))),
                (2, "cat {}".format(
                    select_by_weight( [
                        (1, "/f1.txt"),
                        (1, "/f2.txt"),
                        (1, "/f3.txt")
                    ] )
                )),
                (2, get_random_str(256)),
                (2 if ACTIVE else 0, "python /tmp/mem_test.py " +
                        str(
                            randint(0, 50000),
                        ) + " " +
                        str(randint(1, 1048576) * PAGE_SIZE)
                ),
                (2 if ACTIVE else 0, "bash /tmp/cpu_test.sh " +
                        str(
                            randint(0, cpunr)
                        ) + " " +
                        str(
                            randint(0, 16384)
                        )
                ),
                (2 if ACTIVE_IO else 0, "bash /tmp/io_test.sh " +
                        str(
                            2 ** randint(0, 16)
                        ) + " " +
                        str(
                            randint(0, 50000)
                        )
                )
            ] )
        )


def Isolate():
    return ("isolate", select_equal(["true", "false"]) )

def Respawn():
    return ("respawn", select_equal(["true", "false"]) )

def MaxRespawns():
    return ("max_respawns", str(randint(-1,10)))

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

def EnablePorto():
    return ("enable_porto", select_by_weight( [(2, "true"),
                                               (4, "false"),
                                               (2, "read-only"),
                                               (2, "child-only")] ))

def MemoryLimit():
    return ("memory_limit", str(
            select_by_weight( [
                (3, randint(ramsize / 32, ramsize / 2)),
                (3, randint(ramsize / 2, ramsize)),
                (1, randint(ramsize + 1, sys.maxint)),
                (2, randint(0, ramsize / 32))
            ] )
        )
    )

def MemoryGuarantee():
    return ("memory_guarantee", str(
            select_by_weight( [
                (1, 0),
                (3, randint(ramsize / 32, ramsize / 2)),
                (3, randint(ramsize / 2, ramsize)),
                (1, randint(ramsize + 1, sys.maxint)),
                (2, randint(0, ramsize / 32))
            ] )
        )
    )

def AnonLimit():
    return ("anon_limit", str(
            select_by_weight( [
                (6, randint(ramsize / 32, ramsize) / PAGE_SIZE),
                (1, randint(0, ramsize / 32) / PAGE_SIZE),
                (1, randint(ramsize + 1, sys.maxint) / PAGE_SIZE)
            ] )
        )
    )

def DirtyLimit():
    return ("dirty_limit", str(
            select_by_weight( [
                (6, randint(ramsize / 32, ramsize) / PAGE_SIZE),
                (1, randint(0, ramsize / 32) / PAGE_SIZE),
                (1, randint(ramsize + 1, sys.maxint) / PAGE_SIZE)
            ] )
        )
    )

def RechargeOnPgfault():
    return ("recharge_on_pgfault", select_equal(["true", "false"]) )

def CpuLimit():
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
    return ("cpu_guarantee",
            select_by_weight( [
                (12, select_equal( [
                    str( randint(0, cpunr * 100) ),
                    str( randf() ) + "c"
                ] ) ),
                (1, select_equal(["0", "0c"])),
                (2, select_equal( [
                    str( randint(int(cpunr * 100), sys.maxint) ),
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
    return ("io_policy", select_equal(["","none","rt","high","normal","batch","idle"]) )

def User():
    users = pwd.getpwall()
    porto_members = grp.getgrnam("porto").gr_mem
    return ("user", select_by_weight( [
                (4, porto_members[randint(0, len(porto_members) - 1)] ),
                (1, users[randint(0, len(users) - 1)].pw_name ),
                (1, str(randint(0, 65536)) )
        ] )
    )

def OwnerUser():
    (cmd, value) = User()
    return ("owner_user", value)

def Group():
    groups = grp.getgrall()
    return ("group", select_by_weight( [
                (4, groups[randint(0, len(groups) - 1)].gr_name ),
                (1, str(randint(0, 65536)) )
        ] )
    )

def OwnerGroup():
    (cmd, value) = Group()
    return ("owner_group", value)

def Hostname():
    return ("hostname", get_random_str(32) )

def VirtMode():
    return ("virt_mode", select_by_weight([ (2, "false"), (1, "true") ]) )

def Root(conn):
    vol = select_by_weight( [
        (5, ""),
        (5, targets.our_volume(conn)),
        (1, get_random_dir(VOL_PLACE)),
        (1, get_random_str(256))
    ] )

    return ("root" )

def Ip():
    return ("ip", select_by_weight( [
            (1, ""),
            (1, "kettle ::{};eth0 ::{}".format(randint(0, 65535), randint(0, 65535)))
        ] )
    )

def Net():
    return ("net", select_by_weight( [
            (1, ""),
            (1, "inherited"),
            (1, "L3 kettle"),
            (1, "macvlan eth0 eth0")
        ] )
    )
