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
import subprocess
import signal

def fuzzer_main(tid, iter_num, queue, verbose=False, timeout=180, print_progress=False, active=False):
    try:
        pids = get_portod_pid()
        conn=porto.Connection(timeout=timeout)
        conn.connect()
        random.seed(time.time() + os.getpid())

        fail_cnt = 0
        targets.set_verbose(verbose)
        targets.set_active(active)
        for i in range(0, iter_num):
            if i > 0 and i % 100 == 0:
                check_portod_pid_valid(*pids)
                if print_progress:
                    print "Connection {}: completed {} of {}".format(tid, i, iter_num)

            fail_cnt += select_by_weight(
                [
                    (100, targets.container_action),
                    (10, targets.volume_action),
                    (1, targets.layer_action)
                ]
            )(conn)
    except BaseException as e:
        queue.put(e)
        sys.exit(1)

    print "Connection {} finished: action performed: {}, invalid: {}".format(tid, iter_num, fail_cnt)


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

if os.getuid() != 0:
    print "Running as root required!"
    sys.exit(1)

prepare_fuzzer()

procs=[]
inject_test_utils("/tmp")

start_time = time.time()
pids = get_portod_pid()

for i in range(0, opts.thread_num):
    q = multiprocessing.Queue()
    proc = multiprocessing.Process(target=fuzzer_main, args=(i, opts.iter_num, q,),
                                   name="Connection {}".format(i),
                                   kwargs={"verbose" : opts.verbose,
                                           "timeout" : opts.timeout,
                                           "print_progress" : opts.progress,
                                           "active" : opts.active})
    proc.start()
    procs += [(proc, None, q)]

print "Started {} threads".format(opts.thread_num)
retry = True

try:
    while retry:

        time.sleep(0.3)
        retry = False

        for p in procs:
            if p[1] is None:
                p[0].join(0.001)

                if p[0].exitcode is not None:
                    p = (p[0], p[0].exitcode, p[2])
                    if p[1] > 0:
                        raise BaseException("{} finished with: {}".format(p[0].name, p[2].get()))
                else:
                    retry = True

except (KeyboardInterrupt, SystemExit):
    print "Exiting..."
    for p in procs:
        if p[1] is None:
            p[0].terminate()

    sys.exit(1)

except BaseException as e:
    try:
        print "checking portod pids..."
        check_portod_pid_valid(*pids)
        msg = "\nfuzzer FAIL : {}".format(e)
        print msg
        print "\nportod-master stacktrace:\n\n"
        print_stacktrace(pids[0])
        print "\nportod-slave stacktrace:\n\n"
        print_stacktrace(pids[1])
        print msg
    except BaseException as e2:
        print "fuzzer FAIL : {}".format(e2)

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

