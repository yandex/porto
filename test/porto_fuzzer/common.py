#!/usr/bin/python

import porto
import string
import os
import re
import random
import subprocess

FUZZER_PRIVATE = "porto-fuzzer"

NAME_LIMIT=2
RUN_TIME_LIMIT=10
ITER=10000
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
        slave = int(open("/run/portod.pid").read())
    except:
        slave = None

    try:
        master = int(open("/run/portoloop.pid").read())
    except:
        master = None

    return (master, slave)

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

def print_logged_errors():
    errors = None
    try:
        errors = porto.Connection(timeout=30).GetProperty("/", "porto_stat[errors]")
    except:
        pass
    if errors != "0":
        print "Errors:", errors
        try:
            subprocess.call(["grep", "-wE", "WRN|ERR", "/var/log/portod.log"])
        except:
            pass
