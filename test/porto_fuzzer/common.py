#!/usr/bin/python

import porto
import string
import sys
import os
import random
import subprocess
import tarfile
from random import randint

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

def prepare_fuzzer():
    if not os.path.exists(FUZZER_MNT):
        os.mkdir(FUZZER_MNT)
    
    if not os.path.ismount(FUZZER_MNT):
        subprocess.check_call(["mount", "-t", "tmpfs", "-o", "size=1G", "None", FUZZER_MNT])
    
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
    
def cleanup_fuzzer():
    conn = porto.Connection()

    containers = conn.List()
    while len(containers) > 0 :
        conn.Destroy(containers[0])
        containers = conn.List()

    volumes = conn.ListVolumes()
    while len(volumes) > 0:
        conn.UnlinkVolume(volumes[0].path, "/")
        volumes = conn.ListVolumes()

    if (os.path.ismount(FUZZER_MNT)):
        subprocess.check_call(["umount", FUZZER_MNT])

    if (os.path.exists(FUZZER_MNT)):
        os.rmdir(FUZZER_MNT)

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

def select_equal(elems):
    return elems[randint(0, len(elems)) - 1]

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

def check_portod_pid_valid(master, slave):
    try:
        open("/proc/" + str(master) + "/status").readline().index("portod")
        open("/proc/" + str(slave) + "/status").readline().index("portod-slave")
        return True

    except BaseException as e:
        print "Portod master pid: {} slave pid: {} are invalid!".format(master, slave)
        raise e

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


