#!/usr/bin/python -u

import os
import sys
import pwd
import grp
import re
import string
import time
import random
import subprocess
import multiprocessing
import argparse
import signal
import functools
import tarfile
import porto

from test_common import ConfigurePortod

VERBOSE = False
ACTIVE = False
ACTIVE_IO = False

test_fails = 0

cpunr = 32
ramsize = 32 << 30

FUZZER_PRIVATE = "porto-fuzzer"

NAME_LIMIT = 2
RUN_TIME_LIMIT = 10
PAGE_SIZE=4096
DIRNAME_LIMIT=2
DIR_PATH_LIMIT=3
LAYER_LIMIT=4
LAYERNAME_LIMIT=2

FUZZER_MNT="/tmp/fuzzer_mnt"
VOL_PLACE = FUZZER_MNT + "/place"
VOL_MNT_PLACE = FUZZER_MNT + "/mnt"
VOL_STORAGE = FUZZER_MNT + "/storage"
TAR1 = FUZZER_MNT + "/l1.tar"
TAR2 = FUZZER_MNT + "/l2.tar"
PORTO_KERNEL_PID = None

def randint(a, b):
    return random.randint(a, b)

def randf():
    return random.random()

def get_random_str(length):
    return ''.join(random.choice(string.lowercase) for i in range(length))

def select_by_weight(wlist):
    total = reduce(lambda res, x: res + x[0], wlist, 0)

    selector = randint(0, total - 1)
    accum = 0

    for i in wlist:
        if accum <= selector and selector < accum + i[0]:
            return i[1]

        accum += i[0]

def select_equal(elems, default=None):
    if elems:
        return elems[randint(0, len(elems)) - 1]
    return default

def attach_to_cgroup(dst, pid, move_all=False):
    if not PORTO_KERNEL_PID:
        return

    cgroups = open("/proc/%s/cgroup" % dst).readlines()
    if not move_all:
        cgroups = [cgroups[randint(0, len(cgroups) - 1)]]
    for cgroup in cgroups:
        _, cgtype, path = cgroup.split(":")
        if not cgtype:
            # i.e. unified cgv2
            return
        else:
            if "=" in cgtype:
                cgtype = cgtype.split('=')[1]
            target_path = "/sys/fs/cgroup/%s%s/tasks" % (cgtype, path.rstrip())
        open(target_path, "w").write(str(pid))


def inject_test_utils(path):
    file_path = path + "/mem_test.py"
    f = open(file_path, "w")
    f.write(
"""
import mmap
import sys
import struct
iter = int(sys.argv[1])
size = int(sys.argv[2])
mm = mmap.mmap(-1, size)
xo = 0
for k in range(0, iter):
    for i in range(0, size):
        mm.write(struct.pack("B", i % 256))
    mm.seek(0)
    for i in range(0, size):
        s = mm.read(1)
        xo ^= struct.unpack("B", s)[0]
    print xo
""")
    f.close()
    file_path = path + "/cpu_test.sh"
    f = open(file_path, "w")
    f.write(
"""
for i in `seq 1 $1`; do
    dd if=/dev/urandom bs=65536 count=$2 | md5sum &
done
""")
    f.close()
    file_path = path + "/io_test.sh"
    f = open(file_path, "w")
    f.write(
"""
dd if=/dev/zero of=./zeroes bs=$1 count=$2
""")
    f.close()

def get_portod_pid():
    try:
        return int(open("/run/portod.pid").read())
    except:
        return None

def get_portod_master_pid():
    try:
        return int(open("/run/portoloop.pid").read())
    except:
        return None

def get_property(conn, ct, prop, fallback=None):
    try:
        return conn.GetProperty(ct, prop)
    except:
        return fallback

def check_errors_present(conn, hdr):
    try:
        value = conn.GetProperty("/", "porto_stat[errors]")
    except BaseException as e:
        print "{}failed to check portod error count: {}\n".format(hdr, e),
        raise e

    try:
        assert value == "0"
    except AssertionError as e:
        try:
            ival = int(value)
            assert ival > 0
            print "{}portod logged some error, terminating\n".format(hdr),
        except:
            print "{}portod returned invalid response: {}"\
                  "instead of error count\n".format(hdr, value),

        raise AssertionError("errors \"{}\" != 0".format(value))

def check_warns_present(conn, old):
    try:
        value = conn.GetProperty("/", "porto_stat[warnings]")
    except BaseException as e:
        print "{}failed to check portod warning count: {}\n".format(hdr, e),
        return old

    try:
        assert value == old
    except AssertionError as e:
        print "portod emitted some warnings, see log for details\n",

    return value

def check_portod_pid_valid(master, slave):
    try:
        open("/proc/" + str(master) + "/status").readline().index("portod-master")
        open("/proc/" + str(slave) + "/status").readline().index("portod")
        return True

    except BaseException as e:
        raise BaseException("Portod master pid: {}, slave pid: {} are invalid, "\
                            "{}?".format(master, slave, "not running" if slave is None or\
                            master is None else "restart"))

def create_dir(path, name):
    if not os.path.exists(path + "/" + name):
        #Race between fuzzer processes is possible,
        #so let's check if porto can handle this
        try:
            os.mkdir(path + "/" + name)
        except:
            pass
    return "/" + name

def get_random_dir(base):
    if not os.path.exists(base):
        raise BaseException("Base path does not exists!")

    max_depth = randint(1, DIR_PATH_LIMIT)
    result = base

    for i in range(0, max_depth):
        try:
            subdirs = os.listdir(result)
        except:
            return result

        if len(subdirs) == 0:
            result += create_dir(result, get_random_str(DIRNAME_LIMIT))
        else:
            result += select_by_weight( [
                (1, create_dir(result, get_random_str(DIRNAME_LIMIT))),
                (2, "/" + subdirs[randint(0,len(subdirs) - 1)]) 
            ])

    return result

def print_stacktrace(pid):
    subprocess.call(["gdb", "-ex", "thread apply all bt",
                            "-ex", "thread apply all bt full",
                            "-ex", "set confirm off",
                            "-ex", "quit", "-q", "-p", str(pid)])

def ParseMountinfo(pid="self"):
    ret = {}
    for line in open("/proc/{}/mountinfo".format(pid), "r"):
        l = line.split()
        sep = l.index('-')
        m = { 'mount_id': l[0], 'parent_id': l[1], 'dev': l[2], 'bind': l[3], 'target': l[4], 'flag': l[5].split(','), 'type': l[sep+1], 'source': l[sep+2], 'opt': l[sep+3].split(',') }
        for tag in l[6:sep]:
            if ':' in tag:
                n, v = tag.split(':', 2)
                m[n] = v
            else:
                m[tag] = True
        # override lower mounts
        ret[m['target']] = m
    return ret

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
    return ("virt_mode",
            select_by_weight([
                (40, "app"),
                (20, "os"),
                (10, "job"),
                (10, "host"),
                (1, "false"),
                (1, "true"),
            ]))

def Root(conn):
    vol = select_by_weight( [
        (5, ""),
        (5, our_volume(conn)),
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

def Label():
    return (
        select_by_weight( [
            (1, "TEST.test"),
            (1, ".TEST.test"),
            (1, "TEST.test!"),
            (1, "TEST." + str(randint(0, 100))),
        ] ),
        select_by_weight( [
            (1, ""),
            (1, "!"),
            (1, "a"),
            (1, "a" * 128),
        ] )
    )

def Create(conn, name):
    if VERBOSE:
        print "Creating container: " + name
    conn.Create(name)
    conn.SetProperty(name, 'private', FUZZER_PRIVATE)

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

WaitCache = []

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
            (200, Command),
            (10, Isolate),
#            (20, Respawn),
            (15, MaxRespawns),
            (10, Weak),
            (20, AgingTime),
            (25, EnablePorto),
            (50, MemoryLimit),
            (50, MemoryGuarantee),
            (35, AnonLimit),
            (35, DirtyLimit),
            (30, RechargeOnPgfault),
            (50, CpuLimit),
            (50, CpuGuarantee),
            (30, CpuPolicy),
            (35, IoLimit),
            (35, IoOpsLimit),
            (25, IoPolicy),
            (20, User),
            (20, Group),
            (20, OwnerUser),
            (20, OwnerGroup),
            (12, Hostname),
            (20, VirtMode),
            (30, Net),
            (30, Ip),
            (50, Label),
            (20, functools.partial(Root, conn))
            ]
    )()

    # FIXME: Do not trigger PORTO-488 for a while
    if prop == ("virt_mode", "os"):
        if conn.GetProperty(dest, "root_path") == "/":
            return

    elif prop == ("root", "/"):
        if (
            conn.GetProperty(dest, "root_path") == conn.GetProperty(dest, "root") and
            conn.GetProperty(dest, "virt_mode") == "os"
        ):
            return

    if VERBOSE:
        print "Setting container %s property %s = %s" %(dest, prop[0], prop[1])

    conn.SetProperty(dest, prop[0], prop[1])

def Kill(conn,dest):
    signo = randint(1, int(signal.NSIG) - 1)

    if VERBOSE:
        print "Killing the container: %s with %d" %(dest, signo)

    conn.Kill(dest, signo)

def AttachDState(conn, dest):
    ct_pid = int(conn.GetProperty(dest, "root_pid"))
    attach_to_cgroup(ct_pid, PORTO_KERNEL_PID, move_all=True)

def ImportLayer(conn, place, layerstr):
    tar = select_by_weight([ (1, ""), (3, TAR1), (3, TAR2) ])
    if VERBOSE:
        print "Importing layer {} from {}".format(layerstr, tar)
    try:
        conn.ImportLayer(layerstr, tar, place=place, private_value=FUZZER_PRIVATE)
    except porto.exceptions.NoSpace:
        pass

def MergeLayer(conn, place, layerstr):
    tar = select_by_weight([ (1, ""), (3, TAR1), (3, TAR2) ])
    if VERBOSE:
        print "Merging layer {} from {}".format(layerstr, tar)
    try:
        conn.MergeLayer(layerstr, tar, place=place, private_value=FUZZER_PRIVATE)
    except porto.exceptions.NoSpace:
        pass

def RemoveLayer(conn, place, layerstr):
    if VERBOSE:
        print "Removing layer {}".format(layerstr)
    conn.RemoveLayer(layerstr, place=place)

def CreateVolume(conn, pathstr):

    def select_storage():
        return select_by_weight( [
            (5, None),
            (2, get_random_dir(VOL_STORAGE)),
            (2, get_random_dir(VOL_MNT_PLACE)),
#            (1, get_random_dir(VOL_PLACE + "/porto_volumes")),
#            (1, get_random_dir(VOL_PLACE + "/porto_layers"))
        ] )

    def select_backend():
        return select_by_weight( [
            (10, None),
            (5, "bind"),
            (5, "plain"),
            (5, "tmpfs"),
            (2, "hugetmpfs"),
            (7, "overlay"),
#            (3, "quota"),
            (3, "native"),
            (3, "loop"),
        ] )

    def select_size():
        return select_by_weight( [
            (80, None),
            (5, "0"),
            (5, "1M"),
            (5, "1073741824"),
            (5, "1P"),
        ] )

    if VERBOSE:
        print "Creating volume: {}".format(pathstr)

    kwargs = {}
    storage = select_storage()
    if storage is not None:
        kwargs["storage"] = storage

    backend = select_backend()
    if backend is not None:
        kwargs["backend"] = backend

    place = select_place()
    if place is not None:
        kwargs["place"] = place

    layers = select_layers(conn, place)
    if layers is not None:
        kwargs["layers"] = layers

    for prop in ['space_limit', 'inode_limit', 'space_guarantee', 'inode_guarantee']:
        size = select_size()
        if size is not None:
            kwargs[prop] = size

    kwargs['private'] = FUZZER_PRIVATE

    conn.CreateVolume(pathstr, **kwargs)

def UnlinkVolume(conn, pathstr):
    ct = select_volume_container(conn)

    if VERBOSE:
        print "Unlinking volume: {} from container: {}".format(pathstr, ct)

    if pathstr is None:
        pathstr = ""

    target = select_by_weight( [
       (10, None),
       (1, get_random_dir(FUZZER_MNT)),
       ] )

    strict = select_by_weight( [
       (10, False),
       (1, True),
       ] )

    conn.UnlinkVolume(pathstr, ct, target=target, strict=strict)

def LinkVolume(conn, pathstr):

    ct = select_volume_container(conn)

    if VERBOSE:
        print "Linking volume: {} to container {}".format(pathstr, ct)

    if pathstr is None:
        pathstr = ""

    if ct is None:
        ct = ""

    target = select_by_weight( [
       (10, None),
       (1, get_random_dir(FUZZER_MNT)),
       ] )

    read_only = select_by_weight( [
       (10, False),
       (1, True),
       ] )

    conn.LinkVolume(pathstr, ct, target=target, read_only=read_only)

def random_container():
    return FUZZER_PRIVATE + '-' + get_random_str(NAME_LIMIT)

def our_containers(conn):
    return conn.List(mask=FUZZER_PRIVATE+"-***")

def our_container(conn):
    return select_equal(our_containers(conn), random_container())

def our_volumes(conn):
    our = []
    for v in conn.ListVolumes():
        try:
            if v.GetProperty('private') == FUZZER_PRIVATE:
                our.append(v.path)
        except porto.exceptions.PortoException:
            pass
    return our

def our_volume(conn):
    return select_equal(our_volumes(conn), "")

def our_layers(conn, place=None):
    our = []
    for l in conn.ListLayers(place=place):
        try:
            if l.GetPrivate() == FUZZER_PRIVATE:
                our.append(l.name)
        except porto.exceptions.PortoException:
            pass
    return our

def all_layers(conn, place=None):
    return [ l.name for l in conn.ListLayers(place=place) ]

def get_random_layers(conn, place=None):
    max_depth = randint(1, LAYER_LIMIT)
    result = []
    layers = all_layers(conn, place)
    for i in range(0, max_depth):
        result.append(select_by_weight( [
            (3, select_equal(layers, get_random_str(LAYERNAME_LIMIT))),
            (1, get_random_str(LAYERNAME_LIMIT))
        ]))

    return result

def select_layers(conn, place=None):
    return select_by_weight( [
        (10, []),
        (8, ["ubuntu-precise"]),
        (5, get_random_layers(conn, place)),
    ] )

def select_container(conn):
    return select_by_weight( [
        (1, lambda conn: "/"),
        (1, lambda conn: "self"),
        (50, our_container),
        (20, lambda conn: random_container()),
        (10, lambda conn: random_container() + "/" + random_container()),
        (5, lambda conn: random_container() + "/" + random_container() + "/" + random_container()),
        (2, lambda conn: our_container(conn) + "/" + random_container()),
        (1, lambda conn: our_container(conn) + "/" + random_container() + "/" + random_container()),
    ] )(conn)

def select_volume_container(conn):
    return select_by_weight( [
        (10, "/"),
        (10, "self"),
        (10, None),
        (10, "***"),
        (100, select_container(conn))
    ] )

def select_volume(conn):
    return select_by_weight( [
        (15, None),
        (20, our_volume(conn)),
        (30, get_random_dir(VOL_MNT_PLACE))
    ] )

def volume_action(conn):
    select_by_weight( [
        (1, CreateVolume),
        (2, UnlinkVolume),
        (2, LinkVolume),
    ] )(conn, select_volume(conn))

def select_place():
    return select_by_weight( [
        (1, None),
        (1, VOL_PLACE)
    ] )

def layer_action(conn):
    place = select_place()
    name = select_by_weight( [
        (5, select_equal(our_layers(conn, place), get_random_str(LAYERNAME_LIMIT))),
        (5, get_random_str(LAYERNAME_LIMIT))
    ] )
    select_by_weight( [
        (1, ImportLayer),
        (2, MergeLayer),
        (2, RemoveLayer)
    ] )(conn, place, name)

def container_action(conn):
    select_by_weight( [
        (75, Create),
        (20, Destroy),
        (150, SetProperty),
        (40, Start),
        (30, Stop),
#       (15, Pause),
#       (15, Resume),
        (15, Wait),
        (25, Kill),
        (25, AttachDState)
    ] )(conn, select_container(conn))

def prepare_fuzzer():
    global PORTO_KERNEL_PID

    if not os.path.exists(FUZZER_MNT):
        os.mkdir(FUZZER_MNT)

    if os.path.ismount(FUZZER_MNT):
        subprocess.check_call(["umount", "-l", FUZZER_MNT])

    subprocess.check_call(["mount", "-t", "tmpfs", "-o", "size=512M", "None", FUZZER_MNT])

    verify_paths = [VOL_MNT_PLACE, VOL_PLACE, VOL_STORAGE,
                    VOL_PLACE + "/porto_volumes", VOL_PLACE + "/porto_layers"]

    for p in verify_paths:
        if not os.path.exists(p):
            os.mkdir(p)

    open(FUZZER_MNT + "/f1.txt", "w").write("1234567890")
    open(FUZZER_MNT + "/f2.txt", "w").write("0987654321")
    open(FUZZER_MNT + "/f3.txt", "w").write("abcdeABCDE")

    t = tarfile.open(name=TAR1, mode="w")
    t.add(FUZZER_MNT + "/f1.txt", arcname="f1.txt")
    t.add(FUZZER_MNT + "/f2.txt", arcname="f2.txt")
    t.close()

    t = tarfile.open(name=TAR2, mode="w")
    t.add(FUZZER_MNT + "/f1.txt", arcname="f2.txt")
    t.add(FUZZER_MNT + "/f2.txt", arcname="f3.txt")
    t.close()

    if os.environ.get('USE_PORTO_KERNEL', None) == "ON":
        os.system("insmod ./module/porto_kernel.ko")
        PORTO_KERNEL_PID = int(open("/sys/module/porto_kernel/parameters/d_thread_pid").read())

    ConfigurePortod('fuzzer', """
daemon {
    cgroup_remove_timeout_s: 1
}
""")

    ConfigurePortod('fuzzer-core', """
core {
   enable: true
   default_pattern: "/coredumps/%e.%p.%s"
   space_limit_mb: 1024
   timeout_s: 30
}
""")

def cleanup_fuzzer():
    if PORTO_KERNEL_PID:
        os.system("rmmod porto_kernel")

    time.sleep(1)

    conn = porto.Connection()

    for c in our_containers(conn):
        try:
            conn.Destroy(c)
        except porto.exceptions.ContainerDoesNotExist:
            pass

    for v in our_volumes(conn):
        try:
            conn.UnlinkVolume(v, '***')
        except porto.exceptions.VolumeNotFound:
            pass

    for l in our_layers(conn):
        conn.RemoveLayer(l)

    for path, mnt in ParseMountinfo().iteritems():
        if path.startswith(FUZZER_MNT + "/"):
            print "Stale mount: ", path, mnt
            global test_fails
            test_fails += 1

    if (os.path.ismount(FUZZER_MNT)):
        subprocess.check_call(["umount", '-l', FUZZER_MNT])
        subprocess.call('losetup -a | grep "{}/" | cut -d: -f 1 | xargs losetup -d'.format(FUZZER_MNT), shell=True)

    if (os.path.exists(FUZZER_MNT)):
        os.rmdir(FUZZER_MNT)

    ConfigurePortod('fuzzer', "")
    ConfigurePortod('fuzzer-core', "")

def should_stop():
    conn=porto.Connection(timeout=10)
    porto_errors = get_property(conn, "/", "porto_stat[errors]", "0")
    porto_warnings = get_property(conn, "/", "porto_stat[warnings]", "0")
    return porto_errors != "0" or porto_warnings != "0"

def fuzzer_killer(stop, porto_reloads, porto_kills):
    random.seed(time.time() + os.getpid())
    while not stop.is_set():
        target = select_by_weight([
            (90, None),
            (5, True if opts.reload else None),
            (5, False if opts.kill else None),
            ])

        if target is None:
            pid = None
        elif target:
            pid = get_portod_master_pid()
            sig = signal.SIGHUP
            counter = porto_reloads
        else:
            pid = get_portod_pid()
            sig = signal.SIGKILL
            counter = porto_kills

        if pid is not None:
            # reload with D-states should be indicated as error
            attach_to_cgroup(1, PORTO_KERNEL_PID, move_all=True)
            os.kill(pid, sig)
            with counter.get_lock():
                counter.value += 1

        time.sleep(1)

        if should_stop():
            stop.set();

def fuzzer_thread(please_stop, iter_count, fail_count, tseed = time.time() + os.getpid()):
    random.seed(tseed)

    conn=porto.Connection(timeout=opts.timeout)
    fail_cnt = 0
    iter_cnt = 0;
    while True:
        if iter_cnt % 100 == 0 and please_stop.is_set():
            break
        iter_cnt += 1
        try:
            select_by_weight([
                (100, container_action),
                (10, volume_action),
                (1, layer_action)
            ])(conn)
        except porto.exceptions.SocketTimeout:
            if please_stop.is_set():
                break
            fail_cnt += 1
        except porto.exceptions.PortoException:
            fail_cnt += 1
    with iter_count.get_lock():
        iter_count.value += iter_cnt
    with fail_count.get_lock():
        fail_count.value += fail_cnt

parser = argparse.ArgumentParser(description="Porto fuzzing utility")
parser.add_argument("--time", default=60, type=int)
parser.add_argument("--threads", default=100, type=int)
parser.add_argument("--timeout", default=10, type=int)
parser.add_argument("--verbose", action="store_true")
parser.add_argument("--active", action="store_true")
parser.add_argument("--no-kill", dest="kill", action="store_false")
parser.add_argument("--no-reload", dest="reload", action="store_false")
parser.add_argument("--no-cleanup", dest="cleanup", action="store_false")
opts = parser.parse_args()

prepare_fuzzer()

skip_log = os.path.getsize("/var/log/portod.log")

VERBOSE = opts.verbose
ACTIVE = opts.active

procs=[]
inject_test_utils("/tmp")

start_time = time.time()

stop = multiprocessing.Event()
iter_count = multiprocessing.Value('i', 0)
fail_count = multiprocessing.Value('i', 0)
porto_reloads = multiprocessing.Value('i', 0)
porto_kills = multiprocessing.Value('i', 0)

kill_proc = multiprocessing.Process(target=fuzzer_killer, args=(stop, porto_reloads, porto_kills), name="Killer")
kill_proc.start()

tseeds = []
for i in range(0, opts.threads):
    seed = time.time() + os.getpid() + i
    tseeds += [seed]
    print "Fuzzer Thread: {} Seed: {}".format(i, seed)

for i in range(0, opts.threads):
    proc = multiprocessing.Process(target=fuzzer_thread, args=(stop, iter_count, fail_count, tseeds[i]))
    proc.start()
    procs += [proc]

stop.wait(opts.time)
stop.set()

kill_proc.join()
for p in procs:
    p.join()

conn = porto.Connection(timeout=30)

print "Running time", time.time() - start_time
print "Iterations", iter_count.value
print "Fails", fail_count.value
print "Reloads", porto_reloads.value
print "Kills", porto_kills.value
print "Errors", get_property(conn, "/", "porto_stat[errors]")
print "Warnings", get_property(conn, "/", "porto_stat[warnings]")
print "PortoStarts", get_property(conn, "/", "porto_stat[spawned]")
print "VolumeLost", get_property(conn, "/", "porto_stat[volume_lost]")
print "ContainerLost", get_property(conn, "/", "porto_stat[restore_failed]")
print "CgErrors", get_property(conn, "/", "porto_stat[cgerrors]")

if get_property(conn, "/", "porto_stat[errors]") != "0":
    test_fails += 1

if get_property(conn, "/", "porto_stat[warnings]") != "0":
    test_fails += 1

print ("--- log ---")
os.system("tail -c +{} /var/log/portod.log | grep -wE 'WRN|ERR|STK'".format(skip_log))
print ("--- end ---")

if opts.cleanup:
    cleanup_fuzzer()

if test_fails != 0:
    sys.exit(1)

