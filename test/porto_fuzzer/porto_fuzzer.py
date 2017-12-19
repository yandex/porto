#!/usr/bin/python -u

from common import *
import targets

import porto

import random
import time
import os
import sys
import multiprocessing
import argparse
import subprocess
import signal
import functools
import traceback
import tarfile


def prepare_fuzzer():
    if not os.path.exists(FUZZER_MNT):
        os.mkdir(FUZZER_MNT)

    if os.path.ismount(FUZZER_MNT):
        subprocess.check_call(["umount", "-l", FUZZER_MNT])

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

    for c in targets.our_containers(conn):
        try:
            conn.Destroy(c)
        except porto.exceptions.ContainerDoesNotExist:
            pass

    for v in targets.our_volumes(conn):
        try:
            conn.UnlinkVolume(v, '***')
        except porto.exceptions.VolumeNotFound:
            pass

    for l in targets.our_layers(conn):
        conn.RemoveLayer(l)

    if (os.path.ismount(FUZZER_MNT)):
        subprocess.check_call(["umount", FUZZER_MNT])

    if (os.path.exists(FUZZER_MNT)):
        os.rmdir(FUZZER_MNT)

def fuzzer_killer(prob, timeout=180, verbose=False):

    if prob == 0.0:
        sys.exit(0)

    #Exit with an error if portod spawns errors after restore

    conn_name = "killer[{}]:".format(os.getpid())

    random.seed(time.time() + os.getpid())
    conn=porto.Connection(timeout=timeout)
    conn.connect()

    warns_value = "0"

    try:
        while True:
            if random.random() < prob:
                conn.disconnect()

                select_by_weight(
                    [
                        (1, functools.partial(os.kill, get_portod_pid()[1], signal.SIGKILL)),
                        (1, functools.partial(os.kill, get_portod_pid()[0], signal.SIGHUP))
                    ]
                )()

                time.sleep(1)

                print "{} portod killed\n".format(conn_name),

                conn.connect()
                check_errors_present(conn, "{} ".format(conn_name))

                if verbose:
                    warns_value = check_warns_present(conn, warns_value)
            else:
                time.sleep(1)

    except BaseException as e:
        print "{} got {}\n".format(conn_name, e),
        sys.exit(1)

def fuzzer_main(tid, iter_num, queue,
                verbose=False, timeout=180, print_progress=False,
                active=False, ignore_failure=False):

    retry = True
    iter_saved = 0
    warns_value = "0"
    conn_name = "fuzzer%02d[%d]:" %(tid, os.getpid())

    print conn_name + " started\n",

    while retry:
        try:
            pids = get_portod_pid()
            conn=porto.Connection(timeout=timeout)
            conn.connect()
            random.seed(time.time() + os.getpid())

            fail_cnt = 0
            targets.set_verbose(verbose)
            targets.set_active(active)
            for i in range(iter_saved, iter_num):
                iter_saved = i

                if i > 0 and i % 100 == 0:
                    if print_progress:
                        print "{} completed {} of {}\n".format(conn_name, i, iter_num),

                    check_portod_pid_valid(*pids)
                    check_errors_present(conn, "{} ".format(conn_name))
                    if verbose:
                        warns_value = check_warns_present(conn, warns_value)

                fail_cnt += select_by_weight(
                    [
                        (100, targets.container_action),
                        (10, targets.volume_action),
                        (1, targets.layer_action)
                    ]
                )(conn)

            retry = False

        except BaseException as e:
            if not ignore_failure or type(e) is AssertionError:
                traceback.print_exc()
                queue.put(e)
                sys.exit(1)
            else:
                iter_saved += 1

    print "{} finished, action performed: {}, invalid: {}\n".format(conn_name, iter_num, fail_cnt),
    try:
        check_errors_present(conn, "{} ".format(conn_name))
        check_warns_present(conn, "0")
    except porto.exceptions.SocketError:
        pass


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
parser.add_argument("--kill_probability", dest="kill_prob", default=0.0, type=float,\
                    help="Probability of fuzzer sending signal to portod, default 0.0")
opts = parser.parse_args()

if os.getuid() != 0:
    print "Running as root required!"
    sys.exit(1)

prepare_fuzzer()

procs=[]
inject_test_utils("/tmp")

start_time = time.time()
pids = get_portod_pid()

kill_prob = opts.kill_prob

kill_proc = None

if kill_prob:
    kill_proc = multiprocessing.Process(target=fuzzer_killer, args=(kill_prob,),
                                          name="Killer",
                                          kwargs={"timeout" : opts.timeout,
                                                  "verbose" : opts.verbose})
    kill_proc.start()

for i in range(0, opts.thread_num):
    q = multiprocessing.Queue()
    proc = multiprocessing.Process(target=fuzzer_main, args=(i, opts.iter_num, q,),
                                   name="fuzzer%02d" %(i),
                                   kwargs={"verbose" : opts.verbose,
                                           "timeout" : opts.timeout,
                                           "print_progress" : opts.progress,
                                           "active" : opts.active,
                                           "ignore_failure" : kill_prob != 0.0})
    proc.start()
    procs += [(proc, None, q)]

retry = True

try:
    while retry:

        time.sleep(0.3)
        retry = False

        if kill_proc is not None:
            kill_proc.join(0.001)

            if kill_proc.exitcode is not None:
                raise BaseException("killer[{}] returns exit code: {},"\
                                    " terminating".format(kill_proc.pid,
                                                          kill_proc.exitcode))

        for p in procs:
            if p[1] is None:
                p[0].join(0.001)

                if p[0].exitcode is not None:
                    p = (p[0], p[0].exitcode, p[2])
                    if p[1] > 0:
                        raise BaseException("{}[{}] finished with: {}".format(p[0].name,
                                                                              p[0].pid, p[2].get()))
                else:
                    retry = True

except (KeyboardInterrupt, SystemExit):
    print "Exiting...\n",
    for p in procs:
        if p[1] is None:
            p[0].terminate()
            p[0].join()

    if kill_proc is not None:
        kill_proc.terminate()
        kill_proc.join()

    sys.exit(1)

except BaseException as e:
    try:
        print "checking portod pids...\n",
        if kill_proc is None:
            check_portod_pid_valid(*pids)
        msg = "\nfuzzer FAIL : {}".format(e)
        print msg
        print "\nportod-master stacktrace:\n\n",
        print_stacktrace(pids[0])
        print "\nportod stacktrace:\n\n",
        print_stacktrace(pids[1])
        print msg
        print_logged_errors()
    except BaseException as e2:
        print "fuzzer FAIL : {}\n".format(e2),

    for p in procs:
        if p[1] is None:
            p[0].terminate()
            p[0].join()

    if kill_proc is not None:
        kill_proc.terminate()
        kill_proc.join()

    sys.exit(1)

if kill_proc is not None:
    kill_proc.terminate()
    kill_proc.join()

running_time = time.time() - start_time
rps = (opts.thread_num * opts.iter_num) / running_time

print "Running time: {}\n".format(running_time),
print "Total average rps: {}\n".format(rps),
print "Finish iterating\n",
print_logged_errors()

if opts.cleanup:
    print "Performing cleanup: ",
    try:
        cleanup_fuzzer()
        print "OK"
    except BaseException as e:
        print "FAIL"
        print e
        sys.exit(1)

print "fuzzer with {} threads and {} iterations pass OK\n".format(opts.thread_num, opts.iter_num),

