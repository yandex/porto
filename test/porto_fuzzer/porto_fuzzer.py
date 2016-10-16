#!/usr/bin/python -u

import porto

import common
from common import *
import targets

import random
from random import randint
from random import random as randf

import time
import os
import sys
import multiprocessing
import argparse


def fuzzer_main(tid, iter_num, verbose=False, timeout=180, print_progress=False, active=False):
    pids = get_portod_pid()
    conn=porto.Connection(timeout=timeout)
    conn.connect()
    random.seed(time.time() + os.getpid())

    fail_cnt = 0
    targets.set_verbose(verbose)
    targets.set_active(active)

    print "Connection {}: started".format(tid)

    for i in range(0, iter_num):
        if i > 0 and i % 100 == 0:
            check_portod_pid_valid(*pids)
            if print_progress:
                print "Connection {}: completed {}".format(tid, i)

        fail_cnt += select_by_weight(
            [
                (100, targets.container_action),
                (10, targets.volume_action),
                (1, targets.layer_action)
            ]
        )(conn)

    print "Connection {} finished: action performed: {}, invalid: {}".format(tid, iter_num, fail_cnt)


prepare_fuzzer()

parser = argparse.ArgumentParser(description="Porto fuzzing utility")
parser.add_argument("thread_num",type=int,\
                    help="Number of connections",\
                    )
parser.add_argument("iter_num",type=int,\
                    help="Number of operations to execute per connection",\
                    )
parser.add_argument("--verbose", dest="verbose", action="store_true",\
                    help="Be verbose")
parser.add_argument("--progress", dest="progress", action="store_true",\
                    help="Report test progress")
parser.add_argument("--timeout", dest="timeout", default=180, type=int,\
                    help="Timeout for each connection in seconds")
parser.add_argument("--active", dest="active", action="store_true",\
                    help="Start some resource-consuming apps in containers")
parser.add_argument("--no-cleanup", dest="cleanup", action="store_false",\
                    help="Remove containers, volumes and place after successful run")
opts = parser.parse_args()

procs=[]
inject_test_utils("/tmp")

start_time = time.time()

for i in range(0, opts.thread_num):
    proc = multiprocessing.Process(target=fuzzer_main, args=(i, opts.iter_num,),
                                   name="Connection {}".format(i),
                                   kwargs={"verbose" : opts.verbose,
                                           "timeout" : opts.timeout,
                                           "print_progress" : opts.progress,
                                           "active" : opts.active})
    proc.start()
    procs += [(proc, None)]

retry = True

try:
    while retry:

        time.sleep(1)
        retry = False

        for p in procs:
            if p[1] is None:
                p[0].join(0.001)

                if p[0].exitcode is not None:
                    p = (p[0], p[0].exitcode)
                    if p[1] > 0:
                        raise BaseException("{} finished with code: {}".format(p[0].name, p[1]))
                else:
                    retry = True

except BaseException as e:
    print "fuzzer FAIL : {}".format(e)

    for p in procs:
        if p[1] is None:
            p[0].terminate()

    sys.exit(1)

running_time = time.time() - start_time
rps = (opts.thread_num * opts.iter_num) / running_time

print "Running time: {}".format(running_time)
print "Total average rps: {}".format(rps)
print "Finish iterating"

if opts.cleanup:
    print "Performing cleanup: ",
    try:
        cleanup_fuzzer()
        print "OK"
    except BaseException as e:
        print "FAIL"
        print e
        sys.exit(1)

print "fuzzer with {} threads and {} iterations pass OK".format(opts.thread_num, opts.iter_num)

