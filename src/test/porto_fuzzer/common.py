#!/usr/bin/python

import porto
import string
import sys
import random
from random import randint

NAME_LIMIT=2
RUN_TIME_LIMIT=10
ITER=10000
PAGE_SIZE=4096


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
